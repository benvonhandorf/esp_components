# audio_stream_http

The HTTP sink for [`audio_stream`](../audio_stream): a microphone as an endless WAV
response, which `curl | ffplay`, VLC, sox and a browser's `<audio>` element play as it
arrives.

It is a helper for a handler, not a server and not a route. The application owns its route
table and its authentication, and depends here on ESP-IDF's `esp_http_server` and on nothing
else of ours.

```c
static esp_err_t handle_stream(httpd_req_t *req)
{
    audio_stream_source_t src = describe_my_microphone();
    audio_stream_want_t want = {0};
    audio_stream_why_t why;
    audio_stream_fmt_t fmt;

    if (audio_stream_http_parse_query(req, &src, &want, &why) != ESP_OK ||   // ?rate=&bits=&ch=
        audio_stream_resolve(&src, &want, &fmt, &why) != ESP_OK) {
        return reply_400_with(why);
    }
    audio_stream_http_options_t opt = {.queue = {.bytes = 1 << 20, .use_psram = true}};
    esp_err_t err = audio_stream_http_serve(req, &src, &fmt, &opt);
    if (err == ESP_OK) {
        return ESP_OK;                     // the response is ours now: send nothing more
    }
    return reply_409_busy();               // AUDIO_STREAM_ERR_BUSY, or anything else
}
```

```sh
curl -su user:password 'http://device/api/audio/stream?ch=0,1&bits=24' | ffplay -nodisp -
curl -su user:password http://device/api/audio/stream | sox -t wav - take.wav trim 0 10
```

## Behaviour worth knowing

- **The handler returns at once.** `serve()` takes an asynchronous copy of the request
  (`httpd_req_async_handler_begin()`) and a task of this component sends the audio, so the
  server's one task, shared with whatever else the application serves, is not held for the
  length of a recording. The server does not close or purge a socket that is in an
  asynchronous request.
- **One listener.** A second request is refused with `AUDIO_STREAM_ERR_BUSY`. So is a request
  that arrives in the instant after a listener left, while its buffer is still closing.
- **A buffer between** the stream task and the socket (see `audio_stream_queue.h`), in
  external RAM when the chip has it: a WiFi stall costs a gap in the audio, never an overrun
  in the receiver.
- **A listener that goes away** ends the stream, frees the microphone and closes its socket.
  `audio_stream_http_stop()` ends it from the application's side; the listener sees the
  response end after the buffered audio.
- **Power save adds stutter.** With WiFi modem sleep on, the radio wakes in bursts and a
  stream stutters; turn it off for streaming.

Statistics (`audio_stream_http_get_stats()`): the listener's address, bytes sent, time, and
the queue's fill, high-water mark and drops.
