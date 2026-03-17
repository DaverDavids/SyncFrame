# syntax=docker/dockerfile:1

FROM python:3.13
WORKDIR /app
COPY . .
RUN apt update && apt install -y mosquitto nano
RUN pip install watchdog 'paho-mqtt<2.0.0' flask pillow observer schedule pillow-heif cryptography

RUN mkdir -p /app/data

VOLUME ["/app/data"]

CMD ["python3", "syncframe-server.py"]
EXPOSE 9369/tcp
EXPOSE 9368
