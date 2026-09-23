idf_component_register(
    SRCS "enigma_usb_host.c"
    INCLUDE_DIRS "include"
    REQUIRES usb usb_host_cdc_acm usb_host_hid
)
