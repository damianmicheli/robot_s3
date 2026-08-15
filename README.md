# Robot S3: asistente de voz local

Un pequeño robot de escritorio con ESP32-S3. Al pulsar el botón, graba cinco segundos con un INMP441, manda el audio a Speaches para transcribirlo y usa ese texto como consulta para un modelo local de Ollama.

Todo el procesamiento pesado ocurre en la red local: la placa solo captura audio, controla la interfaz y realiza las peticiones HTTP. No se envía nada a Internet.

## Qué hace

1. Al iniciar, la pantalla muestra `STAND BY` sobre fondo negro y el LED RGB queda apagado.
2. Una pulsación del botón inicia una captura de cinco segundos.
3. Durante la captura, la pantalla y el LED cambian a rojo y muestran `GRABANDO`.
4. El audio se convierte a WAV PCM mono de 16 kHz y 16 bits.
5. El WAV se envía a Speaches (`/v1/audio/transcriptions`).
6. La transcripción se inserta como mensaje de usuario en una conversación de Ollama (`/api/chat`).
7. La respuesta del LLM se muestra en el monitor serie. La siguiente etapa será reproducirla mediante voz.

Tras terminar la grabación, la pantalla vuelve a `STAND BY` y el LED se apaga aunque la red o el modelo todavía estén procesando.

## Componentes y cableado

| Componente | Señal | GPIO ESP32-S3 |
|---|---|---:|
| Botón | un terminal a GPIO, otro a GND | 4 |
| Pantalla GMT020-02-7P / ST7789V | MOSI | 11 |
|  | SCLK | 12 |
|  | CS | 10 |
|  | DC | 13 |
|  | RST | 14 |
| LED PL9823-F5 | DIN | 15 |
| INMP441 | SCK / BCLK | 16 |
|  | WS / LRCL | 17 |
|  | SD | 18 |
|  | L/R | GND (canal izquierdo) |

Todos los módulos comparten `GND`. El INMP441 y el LED deben funcionar a 3,3 V; no conectar el INMP441 a 5 V.

## Configuración local

Las credenciales y las direcciones de los servicios no se versionan. Están en `main/private_config.h`, que Git ignora.

Si empezás desde una copia limpia, creá ese archivo a partir de `main/private_config.example.h` y completá:

```c
#define WIFI_SSID "tu-red-wifi"
#define WIFI_PASSWORD "tu-contrasena"
#define TRANSCRIPTION_URL "http://IP_DEL_SERVIDOR:8001/v1/audio/transcriptions"
#define TRANSCRIPTION_MODEL "Systran/faster-whisper-small"
#define OLLAMA_CHAT_URL "http://IP_DEL_SERVIDOR:11434/api/chat"
#define OLLAMA_MODEL "qwen3.5:9b"
```

Speaches recibe un formulario `multipart/form-data` con el archivo `recording.wav` y el modelo de transcripción. Ollama recibe un `POST /api/chat` con este criterio de sistema: respuestas breves, naturales y en español argentino, pensadas para voz.

## Compilar y flashear

Desde una terminal ESP-IDF configurada en la raíz del proyecto:

```powershell
idf.py build
idf.py flash monitor
```

También podés usar el botón `Flash Device` de la extensión ESP-IDF de VS Code. Es importante flashear, no solo compilar: la aplicación usa una partición de 1,5 MB para incluir Wi-Fi y HTTP.

El binario generado queda en `build/robot_s3.bin`.

## Monitor serie y prueba

Después de flashear, pulsá el botón y hablá durante los cinco segundos. En el monitor deberías ver, en orden:

```text
BUTTON: Grabacion iniciada
SPEACHES: Transcripcion: ...
OLLAMA: Respuesta: ...
```

Si Speaches u Ollama no responden, el monitor informa el código HTTP o el error de red, mientras la interfaz física vuelve siempre a reposo.

## Organización del código

| Archivo | Responsabilidad |
|---|---|
| `main.c` | Orquesta el flujo botón → audio → transcripción → LLM. |
| `button.c` | Lee el botón con antirrebote por software. |
| `display.c` | Inicializa la pantalla con `esp_lcd` y LVGL; incluye la calibración específica que elimina el ruido del panel. |
| `rgb_led.c` | Controla el PL9823 con el driver oficial `led_strip`. |
| `microphone.c` | Configura I²S y guarda temporalmente los cinco segundos de PCM en PSRAM. |
| `network.c` | Conecta la estación Wi-Fi y administra reintentos. |
| `transcription.c` | Construye el WAV y lo carga a Speaches por HTTP. |
| `llm.c` | Construye el mensaje de Ollama y registra su respuesta. |

El proyecto usa bibliotecas oficiales de ESP-IDF para I²S, Wi-Fi, HTTP, LCD y LED, además de LVGL para la interfaz. Se evitó agregar dependencias redundantes: las partes que quedan manuales son el encabezado WAV y el cuerpo multipart, porque son formatos pequeños y estables.

## Próximo paso

Agregar salida de voz: convertir la respuesta de Ollama a audio y reproducirla con un parlante/amplificador adecuado para la ESP32-S3.
