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
import gc
import traceback

app = Flask(__name__)

# CONFIGURACIÓN DEL MODELO
model_path = "model_1200000.safetensors"
F5TTS_model_cfg = dict(dim=1024, depth=22, heads=16, ff_mult=2, text_dim=512, conv_layers=4)

# SELECCIÓN DE DISPOSITIVO
device = "cuda" if torch.cuda.is_available() else "cpu"
print("Usando dispositivo:", device)

# ✅ CARGAMOS EL MODELO UNA SOLA VEZ
print("🔄 Cargando modelo...")
model = load_model(DiT, F5TTS_model_cfg, model_path, device=device)
vocoder = load_vocoder(device=device)
print("✅ Modelo y vocoder cargados.")

@app.route("/sintetizar", methods=["POST"])
def sintetizar():
    audio_path = None
    intermed_path = None
    ref_audio = None
    audio_np = None

    try:
        if "audio" not in request.files:
            return jsonify({"error": "No se envió el archivo de audio."}), 400

        genero = request.form.get("genero", "desconocido")
        edad = request.form.get("edad", "desconocida")
        print(f"Género: {genero}, Edad: {edad}")

        with tempfile.NamedTemporaryFile(delete=False, suffix=".wav") as temp_audio:
            audio_path = temp_audio.name
            request.files["audio"].save(audio_path)

        ref_text = ""
        ref_audio, ref_text_out = preprocess_ref_audio_text(audio_path, ref_text, device=device)
        gen_text = ref_text_out
        print("Texto transcrito:", gen_text)

        with torch.no_grad():
            audio_np, sr, _ = infer_process(
                ref_audio, ref_text_out, gen_text, model, vocoder, device=device
            )

        with tempfile.NamedTemporaryFile(delete=False, suffix=".wav") as temp_out:
            intermed_path = temp_out.name
            sf.write(intermed_path, audio_np, sr)

        audio = AudioSegment.from_file(intermed_path)
        audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(1)

        buffer = BytesIO()
        audio.export(buffer, format="wav")
        buffer.flush()
        buffer.seek(0)

        print("✅ Audio sintetizado enviado correctamente al cliente.")
        return send_file(buffer, as_attachment=True, download_name="voz.wav", mimetype="audio/wav")

    except Exception as e:
        traceback.print_exc()
        return jsonify({"error": str(e)}), 500

    finally:
        for f in [audio_path, intermed_path]:
            if f and os.path.exists(f):
                os.remove(f)
        for var in ['ref_audio', 'audio_np']:
            if var in locals():
                del locals()[var]
        gc.collect()
        if torch.cuda.is_available():
            torch.cuda.empty_cache()

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
