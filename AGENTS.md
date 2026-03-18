# SyncFrame Agent Guidelines

This file provides instructions for AI agents working on the SyncFrame codebase.

## Project Overview

SyncFrame consists of two main components:
1. **SF-ESP32-Clients**: Arduino/ESP32 firmware for image capture and display
2. **SF-Server**: Python Flask web server for managing and serving images

## Development Commands

### SF-Server (Python Flask Application)

#### Setup
```bash
# Install dependencies (inferring from imports)
pip install flask paho-mqtt pillow pillow-heif schedule watchdog cryptography itsdangerous
```

#### Running
```bash
# Run the server
python SF-Server/syncframe-server.py

# Run with debug mode
export FLASK_APP=SF-Server/syncframe-server.py
export FLASK_ENV=development
flask run
```

#### Testing
No explicit test framework detected. Suggested approaches:
```bash
# If using unittest
python -m unittest discover SF-Server/ -v

# If using pytest
pytest SF-Server/ -v

# Run a specific test file
pytest SF-Server/test_specific.py -v

# Run a specific test function
pytest SF-Server/test_specific.py::test_function_name -v
```

#### Linting & Formatting
```bash
# Using flake8
flake8 SF-Server/

# Using pylint
pylint SF-Server/syncframe-server.py

# Using black (formatting)
black SF-Server/syncframe-server.py

# Using isort (import sorting)
isort SF-Server/syncframe-server.py
```

#### Docker Commands
```bash
# Build Docker image
docker build -t syncframe-server .

# Run Docker container
docker run -p 5000:5000 syncframe-server

# Alternative run scripts (provided)
./SF-Server/docker-run
./SF-Server/docker-run-alt
```

### SF-ESP32-Clients (Arduino Firmware)

#### Build
```bash
# Using Arduino CLI (if installed)
arduino-cli compile --fqbn esp32:esp32:esp32 SF-ESP32-Clients/SF-ESP32-Clients.ino

# Using PlatformIO (alternative)
platformio run --project-dir SF-ESP32-Clients
```

#### Upload
```bash
# Using Arduino CLI
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 SF-ESP32-Clients/SF-ESP32-Clients.ino

# Using PlatformIO
platformio run --target upload --project-dir SF-ESP32-Clients
```

#### Serial Monitor
```bash
# Using Arduino CLI
arduino-cli monitor -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 SF-ESP32-Clients/SF-ESP32-Clients.ino
```

## Code Style Guidelines

### Python (SF-Server)

#### Imports
- Group imports in this order:
  1. Standard library imports
  2. Third-party imports
  3. Local application imports
- Use absolute imports for local modules
- Avoid wildcard imports (`from module import *`)
- Align imports vertically when wrapping:
  ```python
  from flask import (
      Blueprint,
      Flask,
      Response,
      redirect,
      render_template_string,
      request,
      send_file,
      send_from_directory,
      session,
      url_for,
      jsonify,
  )
  ```

#### Formatting
- Follow PEP 8 style guide
- Maximum line length: 88 characters (Black default)
- Use 4 spaces per indentation level
- No trailing whitespace
- Blank lines:
  - 2 blank lines between top-level definitions
  - 1 blank line between method definitions
  - Use blank lines sparingly inside functions to indicate logical sections

#### Types
- Use type hints for function signatures (Python 3.6+)
- Use `typing` module for complex types
- Example:
  ```python
  from typing import List, Optional, Dict, Any
  
  def process_data(items: List[str], config: Optional[Dict[str, Any]] = None) -> bool:
      pass
  ```

#### Naming Conventions
- Modules: `lowercase_with_underscores`
- Classes: `CapWords` (PascalCase)
- Functions: `lowercase_with_underscores`
- Constants: `UPPERCASE_WITH_UNDERSCORES`
- Instance variables: `lowercase_with_underscores`
- Instance methods: `lowercase_with_underscores`
- Parameters: `lowercase_with_underscores`
- Local variables: `lowercase_with_underscores`

#### Error Handling
- Use try/except blocks for specific exceptions
- Avoid bare except: clauses
- Log errors appropriately using the logging module
- Raise exceptions with informative messages
- Example:
  ```python
  try:
      # risky operation
      result = risky_operation()
  except ValueError as e:
      logger.error(f"Value error occurred: {e}")
      raise CustomProcessingError(f"Failed to process data: {e}") from e
  except Exception as e:
      logger.exception(f"Unexpected error in processing: {e}")
      raise
  ```

#### Documentation
- Use docstrings for all public modules, classes, and functions
- Follow Google or NumPy docstring style
- Example:
  ```python
  def process_image(data: bytes) -> Image:
      """Process raw image data into a PIL Image object.
      
      Args:
          data: Raw JPEG image bytes
          
      Returns:
          PIL Image object
          
      Raises:
          ValueError: If data is not valid JPEG format
          IOError: If image cannot be opened
      """
      # implementation
  ```

#### Comments
- Use comments sparingly - code should be self-explanatory
- When needed, use complete sentences
- Update comments when modifying code
- Avoid commented-out code

### Arduino/C++ (SF-ESP32-Clients)

#### Formatting
- Use 2 spaces for indentation (Arduino convention)
- Maximum line length: 100 characters
- Place opening braces on same line as control statement
- Place closing braces on their own line
- Example:
  ```cpp
  if (condition) {
      // do something
  } else {
      // do something else
  }
  ```

#### Naming Conventions
- Variables: `lowercaseWithUnderscores` or `camelCase` (be consistent)
- Functions: `lowercaseWithUnderscores` or `camelCase` (be consistent)
- Constants: `UPPERCASE_WITH_UNDERSCORES`
- Classes: `CapWords` (PascalCase)
- #defines: `UPPERCASE_WITH_UNDERSCORES`

#### Error Handling
- Check return values from functions
- Use assertions for debugging
- Handle error conditions gracefully
- Example:
  ```cpp
  if (!sdCard.begin(SD_CS_PIN)) {
      Serial.println("SD Card initialization failed!");
      return;
  }
  ```

#### Documentation
- Comment complex logic and non-obvious implementations
- Document pin assignments and hardware connections
- Use Doxygen-style comments for public APIs if applicable

## Repository Structure

```
SyncFrame/
├─ .git/
├─ .gitattributes
├─ .gitignore
├─ LICENSE
├─ README.md
├─ syncframe-*.jpg
├─ SF-ESP32-Clients/
│  ├─ SF-ESP32-Clients.ino
│  ├─ board_config.h
│  ├─ config_c3.h
│  ├─ config_s3.h
│  ├─ html.h
│  └─ splash.h
└─ SF-Server/
   ├─ .dockerignore
   ├─ Dockerfile
   ├─ docker-bash
   ├─ docker-build
   ├─ docker-run
   ├─ docker-run-alt
   ├─ index.html
   ├─ photo.800x480.jpg
   ├─ photo.jpg
   ├─ static/
   │  ├─ *.png
   │  ├─ manifest.webmanifest
   │  └─ syncframe.png
   └─ syncframe-server.py
```

## Best Practices

### General
1. Keep functions focused on a single responsibility
2. Limit function length (aim for < 50 lines)
3. Write descriptive variable and function names
4. Prefer composition over inheritance
5. Use constants for magic numbers
6. Handle edge cases and error conditions
7. Write code that is easy to test and maintain

### Python-Specific
1. Use virtual environments for dependency management
2. Follow the Flask application factory pattern when applicable
3. Use environment variables for configuration
4. Implement proper logging instead of print statements
5. Use Flask blueprints for modular applications
6. Implement proper error handlers (404, 500, etc.)

### Arduino-Specific
1. Minimize use of String objects to prevent memory fragmentation
2. Use F() macro for string literals to save RAM
3. Implement proper error checking for hardware initialization
4. Use interrupts judiciously and keep ISRs short
5. Consider power consumption in battery-operated devices
6. Use proper debouncing for mechanical switches

## Troubleshooting

### Common Python Issues
1. Dependency conflicts: Use virtual environments
2. Port already in use: Check what's running on port 5000
3. Missing dependencies: Check import statements and install missing packages
4. Permission issues: Check file permissions, especially for serial ports

### Common Arduino/ESP32 Issues
1. Board not detected: Check USB cable and port selection
2. Upload failed: Ensure correct board and port are selected
3. Serial monitor gibberish: Match baud rate in code and monitor
4. WiFi connection issues: Check SSID and password, ensure network availability
5. Out of memory: Minimize String usage, use PROGMEM for constant data

## Contributing

When contributing to this repository:
1. Follow the existing code style
2. Write clear, descriptive commit messages
3. Keep pull requests focused on a single change
4. Update documentation when changing functionality
5. Consider adding tests for new functionality
6. Test changes thoroughly before submitting

---
*Generated for use with AI coding agents working on the SyncFrame repository*