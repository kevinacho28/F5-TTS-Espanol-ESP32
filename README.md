---
license: cc-by-nc-4.0
library_name: f5-tts
language:
- es
base_model:
- SWivid/F5-TTS
---

# [GitHub Original](https://github.com/jpgallegoar/Spanish-F5)

# F5-TTS Spanish Adaptación para ESP32

## Descripción general
Esta es una implementación personalizada del modelo **F5-TTS** en español, con el objetivo de integrarlo con sistemas embebidos como el **ESP32**, permitiendo la síntesis de voz desde microcontroladores. El proyecto incluye scripts para prueba local con Flask y una interfaz `.ino` para cargar en el ESP32.

**Adaptación realizada por: Kevin R. Landázuri, basada en el proyecto de JPGALLEGO**

---

## Estructura del Proyecto

- `flask_f5_ESP32.py`: Servidor Flask para comunicación con ESP32 y generación de audio.
- `flask_f5_transcribe_convert.py`: Versión Flask para uso desde PC directamente.
- `ESP32_Interface.ino`: Código para el microcontrolador ESP32 que envía texto al servidor Flask.
- `model_1200000.safetensors`: Modelo TTS preentrenado (opcional, puede descargarse externamente).
- `Spanish_F5/`, `transformer_config`, `vocab`: Archivos necesarios para correr el modelo.

---

## Instalación

### Requisitos

- Python 3.10
- pip
- Virtualenv (opcional pero recomendado)

### Pasos

```bash
# Clonar el repositorio
git clone https://github.com/kevinacho28/F5-TTS-Espanol-ESP32.git
cd F5-TTS-Espanol

# Crear entorno virtual
python -m venv venv
source venv/Scripts/activate  # En Windows
# o
source venv/bin/activate  # En Linux/Mac

# Instalar dependencias
pip install -r requirements.txt
