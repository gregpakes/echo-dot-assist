#!/usr/bin/env python3
"""Wake word clips (wakeclip.c, setting "wake_clips"; fork gregpakes/echo-dot-assist) against build/hassmic-host: off,
nothing is sent; on, each request a wake word started arrives at the collector as a WAV with the wake word and the
start of Home Assistant's stream marked, and Home Assistant gets the same audio as with it off."""
import asyncio, io, signal, threading, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from aioesphomeapi import APIClient, VoiceAssistantEventType as Ev
from esphome_common import check, until, sync, speech, run

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT, CLIPS = 16981, 16982, 16983, 16984, 16985, 16986
clips = []


class Collector(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        clips.append((self.path, dict(self.headers), body))
        self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()


async def request(e, cli, mic, chunks, started):
    """one wake word, a second or so of speech, Home Assistant's VAD end and the run's end; what HA got"""
    mic.clear(); chunks.clear(); started.clear()
    e.proc.send_signal(signal.SIGUSR1)
    await asyncio.wait_for(started.wait(), 5)
    await speech(mic)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_START, None)
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_STT_VAD_END, None)
    await sync(cli); await asyncio.sleep(0.3)
    got = (bytes(mic), list(chunks))
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_ERROR, {"code": "x", "message": "end of test pipeline"})
    cli.send_voice_assistant_event(Ev.VOICE_ASSISTANT_RUN_END, None)
    await sync(cli); await asyncio.sleep(0.3)
    return got


async def main(e):
    httpd = ThreadingHTTPServer(("127.0.0.1", CLIPS), Collector)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        cli = APIClient("127.0.0.1", PORT, None)
        await cli.connect(login=True)
        started = asyncio.Event(); mic = bytearray(); chunks = []
        async def handle_start(conv_id, flags, settings, phrase): started.set(); return 0
        async def handle_stop(abort): pass
        async def handle_audio(data, *_): mic.extend(data); chunks.append(len(data))
        cli.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio)
        await sync(cli)

        check(e.pv("wake_clips") in (False, "off", 0), f"wake word clips off by default: {e.pv('wake_clips')!r}")
        await request(e, cli, mic, chunks, started)
        await asyncio.sleep(0.5)
        check(not clips, f"off: nothing sent to the collector ({len(clips)} clips)")

        r = e.page.set(wake_clips="on")
        check(r.get("status", 200) == 200 and e.conf().get("wake_clips") == "on", f"switched on from the settings page and saved: {r} / {e.conf().get('wake_clips')}")
        on_mic, _ = await request(e, cli, mic, chunks, started)
        await until(lambda: clips, 5)
        check(len(clips) == 1, f"on: one clip for the one request ({len(clips)})")
        if not clips: return
        path, h, body = clips[0]
        w = wave.open(io.BytesIO(body))
        n = w.getnframes()
        check(path == "/clip" and h.get("Content-Type") == "audio/wav" and (w.getframerate(), w.getsampwidth(), w.getnchannels()) == (16000, 2, 1),
              f"POST /clip, a 16 kHz 16-bit mono WAV: {path} {h.get('Content-Type')} {w.getframerate()} {w.getsampwidth()} {w.getnchannels()}")
        wb, we, sf = (int(h.get(k, -1)) for k in ("X-Wake-Begin-Ms", "X-Wake-End-Ms", "X-Stream-From-Ms"))
        # the PC's wake word: the 0.6 s up to 128 ms before it was heard; the clip starts 300 ms before it
        check(wb == 300 and we == 772 and sf == we, f"wake word at 300-772 ms, Home Assistant's stream from its end (no level known on the PC): {wb}-{we}, {sf}")
        after = n - sf * 16
        # Home Assistant's stream is untouched (fake_ha_esphome.py checks it with the feature off, its default): the clip
        # holds as many samples after the stream's start as Home Assistant got, the same audio before the gain
        check(abs(after - len(on_mic) // 2) <= 1600, f"after the stream's start, the clip has what Home Assistant got: {after} samples against {len(on_mic) // 2}")
        check(h.get("X-Echo") == "echo-dot" and h.get("X-Keyword") == "" and int(h.get("X-Stream-Start-Ms", 0)) > 1_700_000_000_000,
              f"headers name the Echo, the keyword and the stream's start: {h.get('X-Echo')!r} {h.get('X-Keyword')!r} {h.get('X-Stream-Start-Ms')}")

        r = e.page.set(wake_clips="off"); clips.clear()
        await request(e, cli, mic, chunks, started); await asyncio.sleep(0.5)
        check(not clips and e.conf().get("wake_clips") == "off", f"off again: nothing sent ({len(clips)})")
    finally:
        httpd.shutdown()


if __name__ == "__main__":
    run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_WAKE_CLIP_TARGET": f"127.0.0.1:{CLIPS}"})
