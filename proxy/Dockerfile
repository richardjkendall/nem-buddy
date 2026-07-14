FROM python:3.12-slim
WORKDIR /app
COPY nem_proxy.py /app/nem_proxy.py
# stdlib only — no pip install needed
EXPOSE 8080
ENTRYPOINT ["python3", "/app/nem_proxy.py", "--port", "8080", "--host", "0.0.0.0"]
