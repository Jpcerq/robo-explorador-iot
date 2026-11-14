FROM python:3.13-slim
WORKDIR /app
COPY servidor.py .
USER 10001:10001
EXPOSE 8000
CMD ["python", "-u", "servidor.py"]
