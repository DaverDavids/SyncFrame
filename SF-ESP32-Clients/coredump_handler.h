// coredump_handler.h
#pragma once
#include <WebServer.h>
#include <esp_partition.h>

extern WebServer server;  // references your existing server object

void setupCoredumpRoute() {
    server.on("/coredump", HTTP_GET, []() {
        const esp_partition_t *cd = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
            NULL);

        if (!cd) {
            server.send(404, "text/plain", "No coredump partition found");
            return;
        }

        uint32_t magic = 0;
        esp_partition_read(cd, 0, &magic, sizeof(magic));
        if (magic != 0xE5DECE02) {
            server.send(404, "text/plain", "No valid coredump (partition empty or erased)");
            return;
        }

        // Stream the partition in 4KB chunks
        WiFiClient client = server.client();
        String header = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Content-Disposition: attachment; filename=\"coredump.bin\"\r\n"
                        "Content-Length: " + String(cd->size) + "\r\n"
                        "Connection: close\r\n\r\n";
        client.print(header);

        const size_t CHUNK = 4096;
        uint8_t buf[CHUNK];
        size_t offset = 0;
        while (offset < cd->size) {
            size_t toRead = (cd->size - offset < CHUNK) ? (size_t)(cd->size - offset) : CHUNK;
            esp_partition_read(cd, offset, buf, toRead);
            client.write(buf, toRead);
            offset += toRead;
        }
        client.stop();
    });
}