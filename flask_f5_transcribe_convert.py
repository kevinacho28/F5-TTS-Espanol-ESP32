from flask import Flask, request, send_file, jsonify
import os
import torch
import tempfile
import soundfile as sf
from io import BytesIO
from pydub import AudioSegment
from f5_tts.model import DiT
from f5_tts.infer.utils_infer import (
    load_model,
    load_vocoder,
    preprocess_ref_audio_text,
    infer_process
)

app = Flask(__name__)

# CONFIGURACIÓN DEL MODELO
model_path = "model_1200000.safetensors"
F5TTS_model_cfg = dict(dim=1024, depth=22, heads=16, ff_mult=2, text_dim=512, conv_layers=4)

# SELECCIÓN DE DISPOSITIVO
device = "cuda" if torch.cuda.is_available() else "cpu"
print("Usando dispositivo:", device)

# CARGA DEL MODELO Y VOCODER
vocoder = load_vocoder(device=device)
model = load_model(DiT, F5TTS_model_cfg, model_path, device=device)

@app.route("/sintetizar", methods=["POST"])
def sintetizar():
    if "audio" not in request.files:
        return jsonify({"error": "No se envió el archivo de audio con el nombre 'audio'."}), 400

    audio_file = request.files["audio"]

    with tempfile.NamedTemporaryFile(delete=False, suffix=".wav") as temp_audio:
        audio_path = temp_audio.name
        audio_file.save(audio_path)

    try:
        # 1. Preprocesamiento: transcripción automática
        ref_text = ""
        ref_audio, ref_text_out = preprocess_ref_audio_text(audio_path, ref_text, device=device)
        gen_text = ref_text_out
        print("Texto transcrito:", gen_text)

        # 2. Inferencia
        audio_np, sr, _ = infer_process(
            ref_audio, ref_text_out, gen_text, model, vocoder, device=device
        )

        # 3. Guardar temporalmente la salida
        with tempfile.NamedTemporaryFile(delete=False, suffix=".wav") as temp_out:
            intermed_path = temp_out.name
            sf.write(intermed_path, audio_np, sr)

        # 4. Convertir a 16kHz, mono, 8 bits en memoria
        audio = AudioSegment.from_file(intermed_path)
        audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(1)

        buffer = BytesIO()
        audio.export(buffer, format="wav")
        buffer.seek(0)

        return send_file(
            buffer,
            as_attachment=True,
            download_name="voz_16khz_8bit.wav",
            mimetype="audio/wav"
        )

    except Exception as e:
        return jsonify({"error": str(e)}), 500

    finally:
        # Limpieza de archivos temporales
        for f in [audio_path, intermed_path]:
            if os.path.exists(f):
                try:
                    os.remove(f)
                except Exception as err:
                    print(f"Error al borrar {f}: {err}")

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
