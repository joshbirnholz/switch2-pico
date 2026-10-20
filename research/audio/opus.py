"""Minimal libopus bindings (ctypes) for the research scripts."""
import ctypes

import numpy as np

_lib = ctypes.CDLL("libopus.so.0")
_lib.opus_encoder_create.restype = ctypes.c_void_p
_lib.opus_decoder_create.restype = ctypes.c_void_p
_lib.opus_get_version_string.restype = ctypes.c_char_p

APPLICATION_RESTRICTED_LOWDELAY = 2051   # CELT only
SET_BITRATE = 4002
SET_VBR = 4006

VERSION = _lib.opus_get_version_string().decode()


def toc_info(toc: int) -> str:
    """Describe an Opus TOC byte (RFC 6716 section 3.1)."""
    config = toc >> 3
    if config < 12:
        mode, bw = "SILK", ["NB", "MB", "WB"][config // 4]
        ms = [10, 20, 40, 60][config % 4]
    elif config < 16:
        mode, bw = "hybrid", ["SWB", "FB"][(config - 12) // 2]
        ms = [10, 20][config % 2]
    else:
        mode, bw = "CELT", ["NB", "WB", "SWB", "FB"][(config - 16) // 4]
        ms = [2.5, 5, 10, 20][config % 4]
    return f"{mode} {bw} {ms} ms {'stereo' if toc & 4 else 'mono'} code {toc & 3}"


class Encoder:
    def __init__(self, rate=48000, channels=1, bitrate=64000, cbr=True):
        err = ctypes.c_int()
        self.enc = ctypes.c_void_p(_lib.opus_encoder_create(rate, channels, APPLICATION_RESTRICTED_LOWDELAY,
                                                            ctypes.byref(err)))
        if err.value:
            raise RuntimeError(f"opus_encoder_create: {err.value}")
        self.channels = channels
        _lib.opus_encoder_ctl(self.enc, SET_BITRATE, ctypes.c_int(bitrate))
        _lib.opus_encoder_ctl(self.enc, SET_VBR, ctypes.c_int(0 if cbr else 1))
        self.buf = (ctypes.c_ubyte * 1500)()

    def encode(self, pcm: np.ndarray) -> bytes:
        pcm = np.ascontiguousarray(pcm, dtype=np.int16)
        frame = len(pcm) // self.channels
        n = _lib.opus_encode(self.enc, pcm.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)), frame, self.buf, 1500)
        if n < 0:
            raise RuntimeError(f"opus_encode: {n}")
        return bytes(self.buf[:n])


class Decoder:
    def __init__(self, rate=48000, channels=1):
        err = ctypes.c_int()
        self.dec = ctypes.c_void_p(_lib.opus_decoder_create(rate, channels, ctypes.byref(err)))
        if err.value:
            raise RuntimeError(f"opus_decoder_create: {err.value}")
        self.channels = channels
        self.out = (ctypes.c_int16 * (5760 * channels))()

    def decode(self, packet: bytes | None) -> np.ndarray:
        """Decode one packet; None conceals a lost one. Returns int16 samples,
        or raises ValueError if libopus rejects the packet."""
        if packet:
            buf = (ctypes.c_ubyte * len(packet)).from_buffer_copy(packet)
            n = _lib.opus_decode(self.dec, buf, len(packet), self.out, 5760, 0)
        else:
            n = _lib.opus_decode(self.dec, None, 0, self.out, 960, 0)
        if n < 0:
            raise ValueError(f"opus_decode: {n}")
        return np.frombuffer(self.out, dtype=np.int16, count=n * self.channels).copy()
