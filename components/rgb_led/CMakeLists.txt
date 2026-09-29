# ============================================================================
#  components/rgb_led/CMakeLists.txt
#
#  Описание нашего компонента rgb_led.
# ============================================================================

idf_component_register(
    SRCS "rgb_led.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__led_strip
    # espressif__led_strip — внутреннее имя компонента led_strip из реестра.
)