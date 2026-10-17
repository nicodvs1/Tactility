# ST77922 display + touch controller

Two drivers for the Sitronix `ST77922` TDDI (touch and display driver in one chip):

- **`st77922`** (`sitronix,st77922`) - the display panel over QSPI, driven by ESP-IDF's
  `esp_lcd_st77922` component. The panel has no hardware axis swap and requires draw windows
  whose X start and width are multiples of 4, so the driver keeps a full-screen shadow copy to
  widen unaligned regions.
- **`st77922-touch`** (`sitronix,st77922-touch`) - the in-cell capacitive touch controller, a
  separate I2C device (0x55) with 16-bit register addresses. Talked to directly over I2C.

License: [Apache v2.0](LICENSE-Apache-2.0.md)
