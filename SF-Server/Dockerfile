# syntax=docker/dockerfile:1

FROM python:3.13
WORKDIR /app
COPY . .
RUN apt update
RUN pip install watchdog 'paho-mqtt<2.0.0' flask pillow observer schedule pillow-heif
RUN apt install -y mosquitto nano
#RUN openssl req -x509 -newkey rsa:4096 -nodes -out cert.pem -keyout key.pem -days 33365 -subj /C=US/ST=NY/L=/O=/OU=/CN=local.net

CMD ["python3", "syncframe-server.py"]
EXPOSE 9369/tcp
EXPOSE 9368
