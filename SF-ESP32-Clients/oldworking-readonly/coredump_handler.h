// coredump_handler.h
#pragma once
#include <WebServer.h>
#include <esp_partition.h>

extern WebServer server;

static const esp_partition_t* _getCoredumpPartition() {
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
        NULL);
}

void setupCoredumpRoute() {

    // Download the coredump binary
    server.on("/coredump", HTTP_GET, []() {
        const esp_partition_t *cd = _getCoredumpPartition();
        if (!cd) {
            server.send(404, "text/plain", "No coredump partition found");
            return;
        }

        // Check if partition is all 0xFF (erased flash)
        uint32_t word0 = 0xFFFFFFFF;
        esp_partition_read(cd, 0, &word0, sizeof(word0));
        if (word0 == 0xFFFFFFFF) {
            server.send(404, "text/plain", "No valid coredump (partition empty or erased)");
            return;
        }

        // Read actual dump size from ELF header offset 4 (IDF stores size here)
        // Fall back to full partition size if it looks bogus
        // Replace the dumpSize logic with simply:
        uint32_t dumpSize = cd->size;  // stream entire partition, trim on host side
        esp_partition_read(cd, 4, &dumpSize, sizeof(dumpSize));
        if (dumpSize == 0 || dumpSize > cd->size || dumpSize == 0xFFFFFFFF) {
            dumpSize = cd->size;
        }

        WiFiClient client = server.client();
        String header = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Content-Disposition: attachment; filename=\"coredump.bin\"\r\n"
                        "Content-Length: " + String(dumpSize) + "\r\n"
                        "Connection: close\r\n\r\n";
        client.print(header);

        const size_t CHUNK = 4096;
        uint8_t buf[CHUNK];
        size_t offset = 0;
        while (offset < dumpSize) {
            size_t toRead = min((size_t)(dumpSize - offset), CHUNK);
            esp_partition_read(cd, offset, buf, toRead);
            client.write(buf, toRead);
            offset += toRead;
        }
        client.stop();
    });

    // Erase the coredump partition (so next crash writes fresh)
    server.on("/coredump/erase", HTTP_POST, []() {
        const esp_partition_t *cd = _getCoredumpPartition();
        if (!cd) {
            server.send(404, "text/plain", "No coredump partition found");
            return;
        }
        esp_err_t err = esp_partition_erase_range(cd, 0, cd->size);
        if (err == ESP_OK) {
            server.send(200, "text/plain", "Coredump partition erased");
        } else {
            server.send(500, "text/plain", String("Erase failed: ") + esp_err_to_name(err));
        }
    });

    // Dump first 64 bytes as hex for debugging the magic/header
    server.on("/coredump/peek", HTTP_GET, []() {
        const esp_partition_t *cd = _getCoredumpPartition();
        if (!cd) { server.send(404, "text/plain", "No partition"); return; }
        uint8_t buf[64];
        esp_partition_read(cd, 0, buf, sizeof(buf));
        String out = "First 64 bytes of coredump partition:\n";
        for (int i = 0; i < 64; i++) {
            if (buf[i] < 0x10) out += "0";
            out += String(buf[i], HEX) + " ";
            if ((i + 1) % 16 == 0) out += "\n";
        }
        server.send(200, "text/plain", out);
    });
}