idf_component_register(
    SRCS
    "web_server.c"
    "main.c"
    "animated_modes.c"
    "flip_dot_driver.c"
    "framebuffer.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_http_server esp_http_client esp_timer esp_event nvs_flash driver lwip
    EMBED_FILES
    "web_static/index.html.gz"
    "web_static/assets/app.js.gz"
    "web_static/vite.svg.gz"
)
