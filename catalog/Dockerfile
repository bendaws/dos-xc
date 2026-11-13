FROM python:3.14-slim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 CATALOG_DATA_DIR=/var/lib/dosxc-catalog
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt \
    && groupadd --gid 10001 catalog \
    && useradd --uid 10001 --gid catalog --no-create-home catalog \
    && mkdir -p /var/lib/dosxc-catalog \
    && chown catalog:catalog /var/lib/dosxc-catalog
COPY app.py packages.py ./
COPY templates ./templates
COPY static ./static
USER catalog
EXPOSE 8080
CMD ["gunicorn", "--bind", "0.0.0.0:8080", "--workers", "2", "--timeout", "180", "app:create_app()"]
