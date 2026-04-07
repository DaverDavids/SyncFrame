# Skip the 24-byte IDF header to get the raw ELF
dd if=coredump.bin of=coredump.elf bs=1 skip=24

# Now decode with the raw ELF
python ~/esp/esp-idf/components/espcoredump/espcoredump.py \
    info_corefile \
    --core coredump.elf \
    --core-format elf \
    ./esp32.esp32.esp32s3/SF-ESP32-Clients.ino.elf
