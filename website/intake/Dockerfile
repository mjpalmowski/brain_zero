FROM python:3.13-slim
RUN groupadd --gid 10001 intake && useradd --uid 10001 --gid intake --no-create-home intake
WORKDIR /app
RUN pip install --no-cache-dir "PyJWT[crypto]==2.10.1"
COPY tools/export_compatibility.py tools/validate_compatibility.py tools/prepare_veconfigure.py /app/tools/
COPY website/intake/ /app/website/intake/
RUN mkdir -p /var/lib/brain-zero-intake && chown intake:intake /var/lib/brain-zero-intake
USER 10001:10001
ENV PYTHONDONTWRITEBYTECODE=1
EXPOSE 8080
CMD ["python", "-B", "/app/website/intake/app.py"]
