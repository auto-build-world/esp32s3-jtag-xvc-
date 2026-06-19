with open(r'H:\ESP_PRO\xvc-esp32-main\xvc-esp32-main\tools\xvc-proxy.py', 'rb') as f:
    c = f.read()

old = (
    b'    logger.debug("UART shift: reading %d TDO bytes...", nr_bytes)\r\n'
    b'    ser.timeout = 5.0\r\n'
    b'    try:\r\n'
    b'        tdo = ser.read(nr_bytes)\r\n'
    b'    except serial.SerialException:\r\n'
    b'        return None\r\n'
    b'    if len(tdo) != nr_bytes:\r\n'
    b'        logger.warning("XVC: shift expected %d TDO bytes, got %d: %s",\r\n'
    b'                       nr_bytes, len(tdo), tdo.hex() if tdo else "(none)")\r\n'
    b'        return None\r\n'
    b'    if nbits >= 32 and len(tdo) >= 4:\r\n'
    b'        logger.debug("XVC: shift TDO[0:4]=%s (IDCODE)", tdo[:4].hex())\r\n'
    b'    return bytes(tdo)'
)

new = (
    b'    ser.timeout = 10.0\r\n'
    b'    # Read framed response: 0x54 + len(u16 LE) + TDO\r\n'
    b'    hdr = ser.read(3)\r\n'
    b'    if len(hdr) != 3 or hdr[0] != 0x54:\r\n'
    b'        logger.warning("UART shift: bad frame header 0x%s", hdr.hex() if hdr else "(none)")\r\n'
    b'        return None\r\n'
    b'    frame_len = struct.unpack("<H", hdr[1:3])[0]\r\n'
    b'    if frame_len < 1 or frame_len > MAX_BYTES:\r\n'
    b'        logger.warning("UART shift: invalid frame length %d", frame_len)\r\n'
    b'        return None\r\n'
    b'    tdo = ser.read(frame_len)\r\n'
    b'    if len(tdo) != frame_len:\r\n'
    b'        logger.warning("XVC: shift expected %d TDO bytes, got %d", frame_len, len(tdo))\r\n'
    b'        return None\r\n'
    b'    if nbits >= 32 and len(tdo) >= 4:\r\n'
    b'        logger.debug("XVC: shift TDO[0:4]=%s (IDCODE)", tdo[:4].hex())\r\n'
    b'    return bytes(tdo)'
)

if old in c:
    c = c.replace(old, new)
    with open(r'H:\ESP_PRO\xvc-esp32-main\xvc-esp32-main\tools\xvc-proxy.py', 'wb') as f:
        f.write(c)
    print('OK')
else:
    print('FAIL')
    idx = c.find(b'reading %d TDO')
    print(c[idx-10:idx+250])
