Put GGUF models here.

Default path used by the compose template:

`models/model.gguf`

The default compose template downloads OpenAI gpt-oss-20b GGUF here on first
startup:

`https://huggingface.co/ggml-org/gpt-oss-20b-GGUF/resolve/main/gpt-oss-20b-mxfp4.gguf`

The partial download path is `models/model.gguf.part`; keep it to resume an
interrupted download. Override `LLAMA_MODEL_URL` before compose up to use
another GGUF model.
