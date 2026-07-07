# FF_VF_GRANULATE

Filtro FFMPEG che granula frame precedenti in quello attuale
Progetto Tesi Triennale IUAV Arti Multimediali

## Files

- `vf_granulate.c` — implementazione del filtro

## Integrazione in FFmpeg

Copiare `vf_granulate.c` in `libavfilter/` e aggiungere la voce in
`allfilters.c` e nel `Makefile`.
