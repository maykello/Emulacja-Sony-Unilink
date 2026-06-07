// =============================================================================
// UsbDrive.cpp — USB Host MSC (Mass Storage Class) na ESP32-S3
// =============================================================================
// Obsługuje podłączenie pendrive'a przez natywny port USB-OTG (GPIO 19/20),
// montuje system plików FAT32 przez VFS i udostępnia go jako fs::FS.
//
// Architektura wątkowa:
//   - usb_host_lib_task (core 0): obsługa niskopoziomowego stosu USB
//   - usb_client_task   (core 0): obsługa zdarzeń klienta + callbacki transferów
//   - main loop         (core 1): odczyt plików przez VFS/FatFs → transfery USB
//
// Protokół: USB Mass Storage BBB (Bulk-Only Transport) + SCSI READ(10)
// =============================================================================

#include "UsbDrive.h"

#include <Arduino.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "usb/usb_host.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "ff.h"
#include "vfs_api.h"

// ============================ Stałe ============================
static const char* TAG = "UsbDrive";
#define USB_MOUNT_POINT     "/usb"
#define USB_DISKIO_PDRV     1              // numer dysku FatFs (0 jest zazwyczaj dla SD)
#define USB_XFER_BUF_SIZE   (4096 + 64)    // bufor DMA: 8 sektorów + margines na CSW
#define USB_MAX_SECT_READ   8              // maks. sektorów na jeden transfer SCSI READ
#define USB_TIMEOUT_MS      5000           // timeout transferu USB

#define CBW_SIGNATURE       0x43425355     // 'USBC'
#define CSW_SIGNATURE       0x53425355     // 'USBS'

// ============================ Struktury BOT ============================
// Command Block Wrapper — wysyłany do urządzenia przed każdą komendą SCSI
typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;         // CBW_SIGNATURE
    uint32_t dCBWTag;               // unikatowy tag do sparowania z CSW
    uint32_t dCBWDataTransferLength;// ile bajtów danych w fazie data
    uint8_t  bmCBWFlags;            // 0x80 = IN (device→host), 0x00 = OUT
    uint8_t  bCBWLUN;               // Logical Unit Number (zazwyczaj 0)
    uint8_t  bCBWCBLength;          // długość komendy SCSI (6..16)
    uint8_t  CBWCB[16];             // komenda SCSI (zero-padded do 16 bajtów)
} msc_cbw_t;

// Command Status Wrapper — odpowiedź urządzenia po wykonaniu komendy
typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;         // CSW_SIGNATURE
    uint32_t dCSWTag;               // tag z CBW
    uint32_t dCSWDataResidue;       // ile bajtów NIE przesłano
    uint8_t  bCSWStatus;            // 0=OK, 1=błąd komendy, 2=phase error
} msc_csw_t;

// ============================ Stan modułu ============================
static usb_host_client_handle_t clientHdl = NULL;
static usb_device_handle_t      devHdl    = NULL;
static uint8_t  bulkInEp  = 0;      // adres endpointu Bulk IN
static uint8_t  bulkOutEp = 0;      // adres endpointu Bulk OUT
static uint16_t bulkInMps = 512;    // Max Packet Size endpointu IN

static usb_transfer_t *xferOut = NULL;   // bufor transferu OUT (CBW)
static usb_transfer_t *xferIn  = NULL;   // bufor transferu IN  (data + CSW)
static SemaphoreHandle_t xferSem = NULL; // sygnalizacja zakończenia transferu

static uint32_t cbwTag = 1;              // licznik tagów CBW
static uint32_t diskSectorCount = 0;     // liczba sektorów dysku
static uint32_t diskSectorSize  = 512;   // rozmiar sektora (zazwyczaj 512)
static uint32_t partitionOffset = 0;     // offset partycji FAT32 (LBA) — 0 dla super-floppy, >0 dla MBR

static volatile bool deviceConnected   = false;
static volatile bool filesystemMounted = false;
static volatile int  newDevAddr        = -1;  // adres nowego urządzenia (z callbacka)

static FATFS *fatFs = NULL;
static const char fatDrv[] = "1:";

// ============================ fs::FS wrapper ============================
// Arduino's fs::FS oparte na VFSImpl — mapuje ścieżkę VFS "/usb" na operacje plikowe
class UsbMscFS : public fs::FS {
public:
    UsbMscFS() : FS(FSImplPtr(new VFSImpl())) {}
    void begin()  { _impl->mountpoint(USB_MOUNT_POINT); }
    void end()    { _impl->mountpoint(nullptr); }
};

static UsbMscFS usbFS;

// ====================================================================
//  NISKOPOZIOMOWE TRANSFERY USB
// ====================================================================

// Callback transferu — wywoływany z kontekstu usb_host_client_handle_events()
static void xferCallback(usb_transfer_t *transfer) {
    if (xferSem) xSemaphoreGive(xferSem);
}

// Wyślij dane przez Bulk OUT endpoint (blokujący)
static esp_err_t bulkOut(const void *data, size_t len) {
    if (!xferOut || !devHdl) return ESP_ERR_INVALID_STATE;
    
    memcpy(xferOut->data_buffer, data, len);
    xferOut->num_bytes          = len;
    xferOut->device_handle      = devHdl;
    xferOut->bEndpointAddress   = bulkOutEp;
    xferOut->callback           = xferCallback;
    xferOut->context            = NULL;
    
    esp_err_t err = usb_host_transfer_submit(xferOut);
    if (err != ESP_OK) return err;
    
    if (xSemaphoreTake(xferSem, pdMS_TO_TICKS(USB_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return (xferOut->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
}

// Odbierz dane przez Bulk IN endpoint (blokujący)
// Dane lądują w xferIn->data_buffer, actual_len = ile odebrano
static esp_err_t bulkIn(size_t maxLen, size_t *actualLen) {
    if (!xferIn || !devHdl) return ESP_ERR_INVALID_STATE;
    
    // ESP-IDF USB Host wymaga, aby rozmiar bufora wejściowego IN był wielokrotnością MPS (Max Packet Size)
    size_t alignedLen = ((maxLen + bulkInMps - 1) / bulkInMps) * bulkInMps;
    if (alignedLen > USB_XFER_BUF_SIZE) {
        alignedLen = (USB_XFER_BUF_SIZE / bulkInMps) * bulkInMps;
    }
    
    xferIn->num_bytes           = alignedLen;
    xferIn->device_handle       = devHdl;
    xferIn->bEndpointAddress    = bulkInEp;
    xferIn->callback            = xferCallback;
    xferIn->context             = NULL;
    
    esp_err_t err = usb_host_transfer_submit(xferIn);
    if (err != ESP_OK) return err;
    
    if (xSemaphoreTake(xferSem, pdMS_TO_TICKS(USB_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (xferIn->status != USB_TRANSFER_STATUS_COMPLETED) return ESP_FAIL;
    
    if (actualLen) {
        // Zwracamy rzeczywistą liczbę odebranych danych, ale nie więcej niż użytkownik prosił
        *actualLen = (xferIn->actual_num_bytes > maxLen) ? maxLen : xferIn->actual_num_bytes;
    }
    return ESP_OK;
}

// ====================================================================
//  PROTOKÓŁ BOT (Bulk-Only Transport)
// ====================================================================

// Wyślij CBW (Command Block Wrapper) z komendą SCSI
static esp_err_t botSendCBW(const uint8_t *scsiCmd, uint8_t scsiCmdLen,
                             uint32_t dataLen, bool dataIn) {
    msc_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature           = CBW_SIGNATURE;
    cbw.dCBWTag                 = cbwTag++;
    cbw.dCBWDataTransferLength  = dataLen;
    cbw.bmCBWFlags              = dataIn ? 0x80 : 0x00;
    cbw.bCBWLUN                 = 0;
    cbw.bCBWCBLength            = scsiCmdLen;
    memcpy(cbw.CBWCB, scsiCmd, scsiCmdLen);
    
    return bulkOut(&cbw, sizeof(cbw));
}

// Odbierz CSW (Command Status Wrapper) — weryfikuje sygnaturę i status
static esp_err_t botReceiveCSW() {
    size_t actual = 0;
    esp_err_t err = bulkIn(sizeof(msc_csw_t), &actual);
    if (err != ESP_OK) return err;
    
    msc_csw_t *csw = (msc_csw_t *)xferIn->data_buffer;
    if (actual < sizeof(msc_csw_t) || csw->dCSWSignature != CSW_SIGNATURE) {
        Serial.printf("[%s] CSW: zła sygnatura lub za krótki (%d B)\n", TAG, actual);
        return ESP_FAIL;
    }
    if (csw->bCSWStatus != 0) {
        Serial.printf("[%s] CSW: status=%d (błąd komendy SCSI)\n", TAG, csw->bCSWStatus);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ====================================================================
//  KOMENDY SCSI
// ====================================================================

// TEST UNIT READY — sprawdza czy urządzenie jest gotowe
static esp_err_t scsiTestUnitReady() {
    uint8_t cmd[6] = {0x00};
    esp_err_t err = botSendCBW(cmd, 6, 0, false);
    if (err != ESP_OK) return err;
    return botReceiveCSW();
}

// INQUIRY — identyfikacja urządzenia (opcjonalne, logujemy info)
static esp_err_t scsiInquiry() {
    uint8_t cmd[6] = {0x12, 0, 0, 0, 36, 0}; // INQUIRY, 36 bajtów odpowiedzi
    esp_err_t err = botSendCBW(cmd, 6, 36, true);
    if (err != ESP_OK) return err;
    
    size_t actual = 0;
    err = bulkIn(36, &actual);
    if (err != ESP_OK) return err;
    
    // Log vendor/product (bajty 8-15 = vendor, 16-31 = product)
    char vendor[9] = {0}, product[17] = {0};
    memcpy(vendor,  xferIn->data_buffer + 8,  8);
    memcpy(product, xferIn->data_buffer + 16, 16);
    Serial.printf("[%s] Pendrive: %.8s %.16s\n", TAG, vendor, product);
    
    return botReceiveCSW();
}

// READ CAPACITY(10) — odczytaj rozmiar dysku
static esp_err_t scsiReadCapacity(uint32_t *outSectors, uint32_t *outSectorSize) {
    uint8_t cmd[10] = {0x25};  // READ CAPACITY(10)
    esp_err_t err = botSendCBW(cmd, 10, 8, true);
    if (err != ESP_OK) return err;
    
    size_t actual = 0;
    err = bulkIn(8, &actual);
    if (err != ESP_OK) return err;
    
    uint8_t *r = xferIn->data_buffer;
    // Bajty 0-3: Last LBA (big-endian), bajty 4-7: block size (big-endian)
    uint32_t lastLBA = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
                       ((uint32_t)r[2] << 8)  | r[3];
    uint32_t blkSize = ((uint32_t)r[4] << 24) | ((uint32_t)r[5] << 16) |
                       ((uint32_t)r[6] << 8)  | r[7];
    
    *outSectors    = lastLBA + 1;
    *outSectorSize = (blkSize > 0) ? blkSize : 512;
    
    err = botReceiveCSW();
    return err;
}

// READ(10) — odczytaj sektory z dysku do bufora użytkownika
static esp_err_t scsiRead10(uint32_t lba, uint16_t numSectors, uint8_t *destBuf) {
    uint8_t cmd[10];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x28;  // READ(10)
    cmd[2] = (lba >> 24) & 0xFF;
    cmd[3] = (lba >> 16) & 0xFF;
    cmd[4] = (lba >> 8)  & 0xFF;
    cmd[5] = lba & 0xFF;
    cmd[7] = (numSectors >> 8) & 0xFF;
    cmd[8] = numSectors & 0xFF;
    
    uint32_t dataLen = (uint32_t)numSectors * diskSectorSize;
    
    // Wyślij CBW
    esp_err_t err = botSendCBW(cmd, 10, dataLen, true);
    if (err != ESP_OK) return err;
    
    // Odbierz dane
    size_t actual = 0;
    err = bulkIn(dataLen, &actual);
    if (err != ESP_OK) return err;
    
    // Kopiuj do bufora użytkownika PRZED odczytem CSW
    // (bo CSW nadpisze xferIn->data_buffer)
    memcpy(destBuf, xferIn->data_buffer, actual);
    
    // Odbierz CSW
    return botReceiveCSW();
}

// ====================================================================
//  INTERFEJS DISKIO DLA FatFs
// ====================================================================

static DSTATUS usbDiskInit(BYTE pdrv)   { return deviceConnected ? 0 : STA_NOINIT; }
static DSTATUS usbDiskStatus(BYTE pdrv) { return deviceConnected ? 0 : STA_NOINIT; }

static DRESULT usbDiskRead(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
    if (!deviceConnected) return RES_NOTRDY;
    
    // Przesunięcie sektora o offset partycji (gdy dysk ma tablicę MBR)
    DWORD physSector = sector + partitionOffset;
    
    // Czytaj w porcjach po USB_MAX_SECT_READ sektorów (limit bufora DMA)
    while (count > 0) {
        UINT toRead = (count > USB_MAX_SECT_READ) ? USB_MAX_SECT_READ : count;
        if (scsiRead10(physSector, toRead, buff) != ESP_OK) {
            Serial.printf("[%s] Błąd odczytu sektora %lu\n", TAG, (unsigned long)physSector);
            return RES_ERROR;
        }
        physSector += toRead;
        count      -= toRead;
        buff       += toRead * diskSectorSize;
    }
    return RES_OK;
}

static DRESULT usbDiskWrite(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
    return RES_WRPRT; // Read-only — nie piszemy na pendrive'a
}

static DRESULT usbDiskIoctl(BYTE pdrv, BYTE cmd, void *buff) {
    switch (cmd) {
        case CTRL_SYNC:      return RES_OK;
        case GET_SECTOR_COUNT: *(DWORD *)buff = diskSectorCount - partitionOffset; return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD  *)buff = (WORD)diskSectorSize; return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
        default: return RES_PARERR;
    }
}

// ====================================================================
//  OBSŁUGA ZDARZEŃ USB HOST
// ====================================================================

// Task obsługujący niskopoziomowy stos USB (wymagany przez ESP-IDF)
static void usbHostLibTask(void *arg) {
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

// Callback zdarzeń klienta USB — NIE robimy tu nic ciężkiego,
// tylko zapisujemy flagę. Konfiguracja urządzenia odbywa się na głównym tasku.
static void usbClientEventCb(const usb_host_client_event_msg_t *event, void *arg) {
    switch (event->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            newDevAddr = event->new_dev.address;
            Serial.printf("[%s] Nowe urządzenie USB (addr=%d)\n", TAG, newDevAddr);
            break;
            
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            Serial.printf("[%s] Urządzenie USB odłączone!\n", TAG);
            deviceConnected = false;
            filesystemMounted = false;
            // Uwaga: nie robimy tu cleanup transferów — to wymagałoby synchronizacji
            // z głównym taskiem. Pendrive po ponownym podpięciu zostanie skonfigurowany od nowa.
            break;
    }
}

// Task klienta USB — przetwarza zdarzenia i callbacki transferów
static void usbClientTask(void *arg) {
    while (true) {
        usb_host_client_handle_events(clientHdl, portMAX_DELAY);
    }
}

// ====================================================================
//  KONFIGURACJA URZĄDZENIA MSC (wywoływana z głównego tasku)
// ====================================================================

static bool configureMscDevice(uint8_t devAddr) {
    Serial.printf("[%s] Konfiguruję urządzenie MSC (addr=%d)...\n", TAG, devAddr);
    
    // 1. Otwórz urządzenie
    esp_err_t err = usb_host_device_open(clientHdl, devAddr, &devHdl);
    if (err != ESP_OK) {
        Serial.printf("[%s] Nie mogę otworzyć urządzenia: %s\n", TAG, esp_err_to_name(err));
        return false;
    }
    
    // 2. Przeczytaj deskryptor urządzenia
    const usb_device_desc_t *devDesc;
    usb_host_get_device_descriptor(devHdl, &devDesc);
    Serial.printf("[%s] VID=0x%04X PID=0x%04X\n", TAG, devDesc->idVendor, devDesc->idProduct);
    
    // 3. Przeczytaj deskryptor konfiguracji i znajdź interfejs MSC
    const usb_config_desc_t *cfgDesc;
    usb_host_get_active_config_descriptor(devHdl, &cfgDesc);
    
    const uint8_t *p = (const uint8_t *)cfgDesc;
    int totalLen = cfgDesc->wTotalLength;
    int offset = 0;
    bool foundMsc = false;
    uint8_t ifaceNum = 0;
    bulkInEp = 0;
    bulkOutEp = 0;
    
    while (offset < totalLen) {
        uint8_t dLen  = p[offset];
        uint8_t dType = p[offset + 1];
        if (dLen == 0) break;
        
        // Deskryptor interfejsu: szukamy class=0x08 (Mass Storage),
        // subclass=0x06 (SCSI), protocol=0x50 (BBB = Bulk-Only)
        if (dType == USB_B_DESCRIPTOR_TYPE_INTERFACE && dLen >= 9) {
            uint8_t iClass    = p[offset + 5];
            uint8_t iSubClass = p[offset + 6];
            uint8_t iProtocol = p[offset + 7];
            
            if (iClass == 0x08 && iSubClass == 0x06 && iProtocol == 0x50) {
                foundMsc = true;
                ifaceNum = p[offset + 2];
                Serial.printf("[%s] Znaleziono interfejs MSC #%d\n", TAG, ifaceNum);
            } else {
                foundMsc = false; // następne endpointy nie należą do MSC
            }
        }
        // Deskryptor endpointu: szukamy Bulk IN i Bulk OUT
        else if (dType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && foundMsc && dLen >= 7) {
            uint8_t  epAddr = p[offset + 2];
            uint8_t  epAttr = p[offset + 3];
            uint16_t epMps  = p[offset + 4] | ((uint16_t)p[offset + 5] << 8);
            
            if ((epAttr & 0x03) == 0x02) {  // Bulk transfer type
                if (epAddr & 0x80) {
                    bulkInEp  = epAddr;
                    bulkInMps = epMps;
                    Serial.printf("[%s] Bulk IN:  0x%02X (MPS=%d)\n", TAG, epAddr, epMps);
                } else {
                    bulkOutEp = epAddr;
                    Serial.printf("[%s] Bulk OUT: 0x%02X (MPS=%d)\n", TAG, epAddr, epMps);
                }
            }
        }
        offset += dLen;
    }
    
    if (!foundMsc || bulkInEp == 0 || bulkOutEp == 0) {
        Serial.printf("[%s] BŁĄD: To nie jest urządzenie MSC (brak interfejsu/endpointów)!\n", TAG);
        usb_host_device_close(clientHdl, devHdl);
        devHdl = NULL;
        return false;
    }
    
    // 4. Zarezerwuj interfejs MSC
    err = usb_host_interface_claim(clientHdl, devHdl, ifaceNum, 0);
    if (err != ESP_OK) {
        Serial.printf("[%s] Nie mogę zarezerwować interfejsu: %s\n", TAG, esp_err_to_name(err));
        usb_host_device_close(clientHdl, devHdl);
        devHdl = NULL;
        return false;
    }
    
    // 5. Alokuj bufory transferów DMA
    if (xferOut) { usb_host_transfer_free(xferOut); xferOut = NULL; }
    if (xferIn)  { usb_host_transfer_free(xferIn);  xferIn  = NULL; }
    
    err = usb_host_transfer_alloc(512, 0, &xferOut);  // CBW = 31 bajtów, 512 z zapasem
    if (err != ESP_OK) {
        Serial.printf("[%s] Alokacja xferOut nie powiodła się\n", TAG);
        return false;
    }
    err = usb_host_transfer_alloc(USB_XFER_BUF_SIZE, 0, &xferIn);
    if (err != ESP_OK) {
        Serial.printf("[%s] Alokacja xferIn nie powiodła się\n", TAG);
        usb_host_transfer_free(xferOut); xferOut = NULL;
        return false;
    }
    
    deviceConnected = true;
    
    // 6. Poczekaj aż urządzenie będzie gotowe (niektóre pendrive'y potrzebują chwili)
    Serial.printf("[%s] Czekam na gotowość urządzenia...\n", TAG);
    bool ready = false;
    for (int retry = 0; retry < 20; retry++) {
        if (scsiTestUnitReady() == ESP_OK) {
            ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!ready) {
        Serial.printf("[%s] UWAGA: Urządzenie nie odpowiada na TEST UNIT READY\n", TAG);
        // Kontynuujemy mimo to — niektóre pendrive'y działają mimo błędu TUR
    }
    
    // 7. INQUIRY — identyfikacja (logowanie)
    scsiInquiry();
    
    // 8. READ CAPACITY — rozmiar dysku
    err = scsiReadCapacity(&diskSectorCount, &diskSectorSize);
    if (err != ESP_OK) {
        Serial.printf("[%s] Nie mogę odczytać pojemności dysku!\n", TAG);
        deviceConnected = false;
        return false;
    }
    uint32_t sizeMB = (uint32_t)((uint64_t)diskSectorCount * diskSectorSize / (1024 * 1024));
    Serial.printf("[%s] Dysk: %lu sektorów × %lu B = %lu MB\n", TAG,
                  (unsigned long)diskSectorCount, (unsigned long)diskSectorSize,
                  (unsigned long)sizeMB);
    
    // 8a. Sprawdź czy sektor 0 to MBR/GPT (tablica partycji) czy VBR (bezpośrednio FAT)
    //     Większość pendrive'ów ma MBR lub GPT z partycją FAT32.
    //     FatFs w ESP-IDF domyślnie nie parsuje tych tablic — robimy to ręcznie.
    {
        uint8_t sect[512];
        partitionOffset = 0;
        
        if (scsiRead10(0, 1, sect) == ESP_OK) {
            // Sprawdź sygnaturę 0x55AA na końcu sektora
            bool hasSig = (sect[510] == 0x55 && sect[511] == 0xAA);
            
            // Sprawdź czy to VBR (FAT Boot Sector)
            bool isVBR = (sect[0] == 0xEB || sect[0] == 0xE9);  // JMP instruction
            uint16_t bps = sect[11] | ((uint16_t)sect[12] << 8);
            bool validBPS = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096);
            
            if (hasSig && isVBR && validBPS) {
                // Sektor 0 to bezpośrednio VBR (super-floppy) — partycja od sektora 0
                Serial.printf("[%s] Sektor 0 to VBR (super-floppy) — brak tablicy partycji\n", TAG);
                partitionOffset = 0;
                
            } else if (hasSig) {
                // Sprawdź typ pierwszej partycji w MBR
                uint8_t partType0 = sect[446 + 4];
                
                if (partType0 == 0xEE) {
                    // ===== GPT (GUID Partition Table) =====
                    Serial.printf("[%s] Sektor 0 to GPT Protective MBR — parsowanie GPT...\n", TAG);
                    
                    // Odczytaj nagłówek GPT z sektora 1
                    if (scsiRead10(1, 1, sect) == ESP_OK &&
                        memcmp(sect, "EFI PART", 8) == 0) {
                        
                        // Odczytaj parametry tablicy partycji GPT
                        uint64_t entriesLBA = sect[72] | ((uint64_t)sect[73] << 8) |
                                              ((uint64_t)sect[74] << 16) | ((uint64_t)sect[75] << 24);
                        uint32_t numEntries = sect[80] | ((uint32_t)sect[81] << 8) |
                                              ((uint32_t)sect[82] << 16) | ((uint32_t)sect[83] << 24);
                        uint32_t entrySize  = sect[84] | ((uint32_t)sect[85] << 8) |
                                              ((uint32_t)sect[86] << 16) | ((uint32_t)sect[87] << 24);
                        
                        Serial.printf("[%s] GPT: wpisy od LBA %lu, %lu wpisów × %lu B\n",
                                      TAG, (unsigned long)entriesLBA, (unsigned long)numEntries, (unsigned long)entrySize);
                        
                        // Microsoft Basic Data GUID (FAT/NTFS/exFAT):
                        // EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
                        // W little-endian: A2 A0 D0 EB E5 B9 33 44 87 C0 68 B6 B7 26 99 C7
                        const uint8_t msDataGUID[] = {
                            0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                            0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
                        };
                        
                        // Czytaj wpisy partycji — max 4 sektory (16 wpisów po 128B)
                        uint32_t maxSectors = (numEntries * entrySize + 511) / 512;
                        if (maxSectors > 4) maxSectors = 4;
                        
                        bool found = false;
                        for (uint32_t s = 0; s < maxSectors && !found; s++) {
                            if (scsiRead10((uint32_t)entriesLBA + s, 1, sect) != ESP_OK) break;
                            
                            int entriesPerSector = 512 / entrySize;
                            for (int e = 0; e < entriesPerSector && !found; e++) {
                                uint8_t *gptEntry = &sect[e * entrySize];
                                
                                // Sprawdź czy wpis nie jest pusty (zerowe GUID = pusty)
                                bool isEmpty = true;
                                for (int g = 0; g < 16; g++) {
                                    if (gptEntry[g] != 0) { isEmpty = false; break; }
                                }
                                if (isEmpty) continue;
                                
                                // Start LBA partycji (offset 32, 8 bajtów little-endian)
                                uint32_t startLBA = gptEntry[32] | ((uint32_t)gptEntry[33] << 8) |
                                                    ((uint32_t)gptEntry[34] << 16) | ((uint32_t)gptEntry[35] << 24);
                                uint32_t endLBA   = gptEntry[40] | ((uint32_t)gptEntry[41] << 8) |
                                                    ((uint32_t)gptEntry[42] << 16) | ((uint32_t)gptEntry[43] << 24);
                                
                                bool isMsData = (memcmp(gptEntry, msDataGUID, 16) == 0);
                                
                                Serial.printf("[%s]   GPT wpis %d: start=%lu end=%lu %s\n",
                                              TAG, (int)(s * entriesPerSector + e),
                                              (unsigned long)startLBA, (unsigned long)endLBA,
                                              isMsData ? "(Microsoft Basic Data)" : "");
                                
                                // Akceptuj pierwszą partycję Microsoft Basic Data (FAT32/NTFS)
                                // lub po prostu pierwszą niepustą partycję
                                if (!found && startLBA > 0) {
                                    partitionOffset = startLBA;
                                    found = true;
                                    Serial.printf("[%s] ✓ Używam partycji GPT na sektorze %lu\n",
                                                  TAG, (unsigned long)startLBA);
                                }
                            }
                        }
                        
                        if (!found) {
                            Serial.printf("[%s] UWAGA: Nie znaleziono partycji w GPT!\n", TAG);
                        }
                    } else {
                        Serial.printf("[%s] UWAGA: Nagłówek GPT nieprawidłowy!\n", TAG);
                    }
                    
                } else {
                    // ===== Klasyczny MBR =====
                    Serial.printf("[%s] Sektor 0 to MBR — szukam partycji FAT32...\n", TAG);
                    
                    for (int p = 0; p < 4; p++) {
                        uint8_t *entry = &sect[446 + p * 16];
                        uint8_t partType = entry[4];
                        uint32_t startLBA = entry[8]  | ((uint32_t)entry[9] << 8) |
                                           ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
                        uint32_t partSize = entry[12] | ((uint32_t)entry[13] << 8) |
                                           ((uint32_t)entry[14] << 16) | ((uint32_t)entry[15] << 24);
                        
                        if (partType != 0x00) {
                            Serial.printf("[%s]   Partycja %d: typ=0x%02X start=%lu size=%lu\n",
                                          TAG, p, partType, (unsigned long)startLBA, (unsigned long)partSize);
                        }
                        
                        // Typy FAT: 0x01 FAT12, 0x04/0x06/0x0E FAT16, 0x0B/0x0C FAT32
                        if (partitionOffset == 0 && startLBA > 0 && partSize > 0 &&
                            (partType == 0x01 || partType == 0x04 || partType == 0x06 ||
                             partType == 0x0B || partType == 0x0C || partType == 0x0E)) {
                            partitionOffset = startLBA;
                            Serial.printf("[%s] ✓ Znaleziono partycję FAT (typ 0x%02X) na sektorze %lu\n",
                                          TAG, partType, (unsigned long)startLBA);
                        }
                    }
                    
                    if (partitionOffset == 0) {
                        Serial.printf("[%s] UWAGA: Nie znaleziono partycji FAT w MBR!\n", TAG);
                    }
                }
            } else {
                Serial.printf("[%s] UWAGA: Sektor 0 bez sygnatury 0x55AA!\n", TAG);
            }
        } else {
            Serial.printf("[%s] UWAGA: Nie mogę odczytać sektora 0!\n", TAG);
        }
    }
    
    // 9. Zarejestruj sterownik dysku w FatFs
    static const ff_diskio_impl_t usbDiskio = {
        .init   = usbDiskInit,
        .status = usbDiskStatus,
        .read   = usbDiskRead,
        .write  = usbDiskWrite,
        .ioctl  = usbDiskIoctl,
    };
    ff_diskio_register(USB_DISKIO_PDRV, &usbDiskio);
    
    // 10. Zarejestruj VFS FAT (mapuje ścieżkę "/usb" na dysk FatFs "1:")
    err = esp_vfs_fat_register(USB_MOUNT_POINT, fatDrv, 5, &fatFs);
    if (err != ESP_OK) {
        Serial.printf("[%s] esp_vfs_fat_register błąd: %s\n", TAG, esp_err_to_name(err));
        deviceConnected = false;
        return false;
    }
    
    // 11. Zamontuj system plików FAT32
    FRESULT fres = f_mount(fatFs, fatDrv, 1);
    if (fres != FR_OK) {
        Serial.printf("[%s] f_mount błąd: %d (pendrive musi być FAT32!)\n", TAG, fres);
        esp_vfs_fat_unregister_path(USB_MOUNT_POINT);
        deviceConnected = false;
        return false;
    }
    
    // 12. Aktywuj wrapper fs::FS
    usbFS.begin();
    filesystemMounted = true;
    
    Serial.printf("[%s] ✓ System plików zamontowany pod %s\n", TAG, USB_MOUNT_POINT);
    return true;
}

// ====================================================================
//  PUBLICZNE API
// ====================================================================

bool usbDriveInit() {
    Serial.printf("[%s] Inicjalizacja USB Host...\n", TAG);
    
    // Semafor do synchronizacji transferów USB
    xferSem = xSemaphoreCreateBinary();
    if (!xferSem) {
        Serial.printf("[%s] Nie mogę utworzyć semafora!\n", TAG);
        return false;
    }
    
    // 1. Zainstaluj bibliotekę USB Host
    usb_host_config_t hostCfg = {};
    hostCfg.skip_phy_setup = false;
    hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    
    esp_err_t err = usb_host_install(&hostCfg);
    if (err != ESP_OK) {
        Serial.printf("[%s] usb_host_install błąd: %s\n", TAG, esp_err_to_name(err));
        return false;
    }
    
    // 2. Uruchom task obsługujący zdarzenia biblioteki USB Host
    BaseType_t ret = xTaskCreatePinnedToCore(
        usbHostLibTask, "usb_lib", 4096, NULL, 2, NULL, 0);
    if (ret != pdPASS) {
        Serial.printf("[%s] Nie mogę uruchomić tasku usb_lib!\n", TAG);
        return false;
    }
    
    // 3. Zarejestruj klienta USB Host
    usb_host_client_config_t clientCfg = {};
    clientCfg.is_synchronous = false;
    clientCfg.max_num_event_msg = 5;
    clientCfg.async.client_event_callback = usbClientEventCb;
    clientCfg.async.callback_arg = NULL;
    
    err = usb_host_client_register(&clientCfg, &clientHdl);
    if (err != ESP_OK) {
        Serial.printf("[%s] usb_host_client_register błąd: %s\n", TAG, esp_err_to_name(err));
        return false;
    }
    
    // 4. Uruchom task klienta (przetwarza zdarzenia + callbacki transferów)
    ret = xTaskCreatePinnedToCore(
        usbClientTask, "usb_cli", 4096, NULL, 3, NULL, 0);
    if (ret != pdPASS) {
        Serial.printf("[%s] Nie mogę uruchomić tasku usb_cli!\n", TAG);
        return false;
    }
    
    Serial.printf("[%s] USB Host gotowy. Podłącz pendrive'a do portu USB-OTG...\n", TAG);
    
    // 5. Czekaj na podpięcie pendrive'a (max 5 sekund)
    //    setup() jeszcze nie ma attachInterrupt, więc delay() jest bezpieczne
    for (int i = 0; i < 50; i++) {
        if (newDevAddr >= 0) {
            int addr = newDevAddr;
            newDevAddr = -1;
            if (configureMscDevice(addr)) {
                return true;
            }
        }
        delay(100);
    }
    
    if (!filesystemMounted) {
        Serial.printf("[%s] Pendrive nie wykryty w ciągu 5s — emulator ruszy bez dźwięku.\n", TAG);
        Serial.printf("[%s] Możesz podpiąć pendrive'a w dowolnym momencie (hot-plug).\n", TAG);
    }
    
    return true; // USB Host działa, nawet jeśli pendrive jeszcze nie podpięty
}

bool usbDriveIsMounted() {
    // Sprawdź hot-plug: jeśli nowe urządzenie zostało podpięte po init
    if (newDevAddr >= 0 && !filesystemMounted) {
        int addr = newDevAddr;
        newDevAddr = -1;
        configureMscDevice(addr);
    }
    return filesystemMounted;
}

fs::FS& usbDriveGetFS() {
    return usbFS;
}
