#ifndef HTML_H
#define HTML_H

const char CUSTOM_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>RFID Display Setup</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
            margin: 0;
        }
        .container {
            background: white;
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 10px 25px rgba(0,0,0,0.2);
            max-width: 400px;
            width: 90%;
        }
        h1 {
            color: #667eea;
            text-align: center;
            margin-bottom: 20px;
        }
        .info {
            background: #f0f0f0;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
        }
        .info p {
            margin: 5px 0;
            font-size: 14px;
        }
        .status {
            text-align: center;
            color: #666;
            margin-top: 15px;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>RFID Display</h1>
        <div class="info">
            <p><strong>Device:</strong> ESP32-C3 SuperMini</p>
            <p><strong>Hostname:</strong> rfid.local</p>
            <p><strong>Status:</strong> Connected</p>
        </div>
        <div class="status">
            <p>Configure WiFi credentials using the WiFiManager portal if connection fails.</p>
        </div>
    </div>
</body>
</html>
)rawliteral";

#endif
