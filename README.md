# FF_VF_GRANULATE Italiano

Filtro FFMPEG (Granulatore Video) che granula frame precedenti in quello attuale,
Progetto Tesi Triennale IUAV Arti Multimediali

## Files

- `vf_granulate.c` — implementazione del filtro

## Integrazione in FFmpeg

Copiare `vf_granulate.c` in `libavfilter/` e aggiungere la voce in
`allfilters.c` "OBJS-$(CONFIG_GRANULATE_FILTER) += vf_granulate.o"
e nel `Makefile` "extern const FFFilter ff_vf_granulate;".
Nella root di ffmpeg, mandare comando "./configure" e poi "make".

## Parametri e utilizzo

`mode` : tipo di grani da usare
    0 = pixel : grani interi (default)
    1 = interlaced h : grani a righe alterne
    2 = interlaced v : grani a colonne alterne
    3 = dither : grani che alternano pixel originali e granulati in modo casuale

`zoom` : zoom sul grano (int 1 - 256)

`zoom_offset_time` : numero di frame per i quali l'offset sul grano zoomato rimane fisso (int 0 - MAX)

`buffer` : numero di frame archiviati nel buffer fifo degli ultimi frame da cui vengono pescati grani (int 1 - 8192)

`grain_w` : larghezza in pixel di ogni grano (int 0 - 8192)

`grain_h` : altezza in pixel di ogni grano  (int 0 - 8192)

`fullscreen` : toggle larghezza e altezza uguali al frame (int 0 - 1 default)

`var_size` : larghezza e altezza modificati casualmente, rimanendo nei valori tra 1 e grain_w e grain_h (int 0 - 1)

`n_grains` : numero di grani per frame (int 0 - MAX)

`ghosting` : divisione dei componenti
    0 = no ghosting : nessun ghosting (default)
    1 = luma ghosting : solo componente luma usata per granulare
    2 = chroma ghosting : solo componente chtoma usata per granulare

`static_grains` : toggle per fissare ogni grano a delle coordinate specifiche (int 0 - 1)

`grains_reset_time` : numero di frame prima del reset delle coordinate fisse (int 0 - MAX)

`delay` : numero di frame per il quale mantenere un delay fisso nel buffer (int 0 default - 8162)

`seed` : seed per AVlfg (0 default - MAX)

Per usare il filtro ./ffmpeg -i "path file di input" -vf "granulate=parametro1=x:parametro2=y..." "path di output"


# FF_VF_GRANULATE English

FFMPEG Filter (Video Granulator) that granulates past frames in current frame

## Files

- `vf_granulate.c` — filter implementation

## FFmpeg compilation

Copy `vf_granulate.c` in `libavfilter/` add record to
`allfilters.c` "OBJS-$(CONFIG_GRANULATE_FILTER) += vf_granulate.o"
and to `Makefile` "extern const FFFilter ff_vf_granulate;".
Go back to ffmpeg root and send command "./configure" and then "make".

## Parameters and usage

`mode` : type of grains
    0 = pixel : rectangular grains (default)
    1 = interlaced h : granulates every other line
    2 = interlaced v : granulates every other column
    3 = dither : sets random interlacing of granulated pixels and original pixels 

`zoom` : in-grain-zoom (int 1 - 256)

`zoom_offset_time` : number of frames before the offset of the zoom is reset (int 0 - MAX)

`buffer` : fifo - number of frames archived (int 1 - 8192)

`grain_w` : width of each grains in pixels (int 0 - 8192)

`grain_h` : height of each grain in pixels (int 0 - 8192)

`fullscreen` : toggle width and height equal to frame size (int 0 - 1 default)

`var_size` : toggle random grain size, uses width and height as max value (int 0 - 1)

`n_grains` : number of grains per frame (int 0 - MAX)

`ghosting` : isolate components
    0 = no ghosting : no ghosting (default)
    1 = luma ghosting : only luma granulation
    2 = chroma ghosting : only chroma granulation

`static_grains` : toggle to fix each grain to specific coordinates (int 0 - 1)

`grains_reset_time` : number of frames before the fixed grain position is reset (int 0 - MAX)

`delay` : number of frames before refresh of fixed buffer delay (int 0 default - 8192)

`seed` : set seed for AVlfg (0 default - MAX)


Filter usage:
./ffmpeg -i "video path" -vf "granulate=parameter1=x:parameter2=y..." "output path"
