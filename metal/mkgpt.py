import struct, zlib, os

esp = open('/tmp/esp.img','rb').read()
SECT = 512
esp_sectors = len(esp)//SECT
esp_start = 2048
disk_sectors = esp_start + esp_sectors + 2048     # room for backup GPT + slack
disk = bytearray(disk_sectors*SECT)

# --- ESP payload ---
disk[esp_start*SECT: esp_start*SECT+len(esp)] = esp

# --- Protective MBR (LBA0) ---
mbr = bytearray(512)
# one 0xEE partition covering the disk (minus MBR)
part = struct.pack('<B3sB3sII', 0x00, b'\x00\x02\x00', 0xEE, b'\xff\xff\xff',
                   1, min(disk_sectors-1, 0xFFFFFFFF))
mbr[446:446+16] = part
mbr[510] = 0x55; mbr[511] = 0xAA
disk[0:512] = mbr

ESP_GUID = bytes([0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B])
DISK_GUID = os.urandom(16); PART_GUID = os.urandom(16)
first_usable = 34
last_usable  = disk_sectors - 34

# --- partition entry array (128 entries * 128 bytes = 32 sectors) ---
entry = bytearray(128)
entry[0:16]  = ESP_GUID
entry[16:32] = PART_GUID
struct.pack_into('<Q', entry, 32, esp_start)
struct.pack_into('<Q', entry, 40, esp_start + esp_sectors - 1)
struct.pack_into('<Q', entry, 48, 0)
entry[56:56+len('EFI System')*2] = 'EFI System'.encode('utf-16-le')
parr = bytes(entry) + b'\x00'*(128*128 - 128)
parr_crc = zlib.crc32(parr) & 0xFFFFFFFF

def gpt_header(cur, bak, parr_lba):
    h = bytearray(92)
    h[0:8] = b'EFI PART'
    struct.pack_into('<I', h, 8, 0x00010000)
    struct.pack_into('<I', h, 12, 92)
    struct.pack_into('<I', h, 16, 0)          # header crc (fill later)
    struct.pack_into('<I', h, 20, 0)
    struct.pack_into('<Q', h, 24, cur)
    struct.pack_into('<Q', h, 32, bak)
    struct.pack_into('<Q', h, 40, first_usable)
    struct.pack_into('<Q', h, 48, last_usable)
    h[56:72] = DISK_GUID
    struct.pack_into('<Q', h, 72, parr_lba)
    struct.pack_into('<I', h, 80, 128)
    struct.pack_into('<I', h, 84, 128)
    struct.pack_into('<I', h, 88, parr_crc)
    crc = zlib.crc32(bytes(h)) & 0xFFFFFFFF
    struct.pack_into('<I', h, 16, crc)
    return bytes(h) + b'\x00'*(SECT-92)

primary_parr_lba = 2
backup_parr_lba  = disk_sectors - 33
# primary header LBA1, backup at last LBA
prim = gpt_header(1, disk_sectors-1, primary_parr_lba)
bak  = gpt_header(disk_sectors-1, 1, backup_parr_lba)

disk[1*SECT:1*SECT+SECT] = prim
disk[primary_parr_lba*SECT: primary_parr_lba*SECT+len(parr)] = parr
disk[backup_parr_lba*SECT: backup_parr_lba*SECT+len(parr)] = parr
disk[(disk_sectors-1)*SECT:(disk_sectors-1)*SECT+SECT] = bak

open('build/outrun-install.img','wb').write(disk)
print('disk sectors:', disk_sectors, '(%.1f MiB)'%(disk_sectors*SECT/1048576))
print('ESP LBA %d..%d (%d sectors)'%(esp_start, esp_start+esp_sectors-1, esp_sectors))
print('wrote build/outrun-install.img')
