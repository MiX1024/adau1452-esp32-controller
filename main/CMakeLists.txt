idf_component_register(SRCS "main.c" "adau1452.c" "st7789.c" "ec11.c" "ui.c" "net.c"
                            "sigma_tcp.c" "dsp_image.c" "logbuf.c" "dsp_graph.c" "eq_store.c"
                            "usbi.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "web/index.html")
