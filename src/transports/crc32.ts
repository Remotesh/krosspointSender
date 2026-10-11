/**
 * CRC-32 (IEEE 802.3): polynomial 0xEDB88320 (reflected), init 0xFFFFFFFF,
 * final xor 0xFFFFFFFF. This is the checksum of the CrossPoint BLE file
 * transfer protocol (docs/BLE_PROTOCOL.md). Test vector:
 * crc32("123456789") = 0xCBF43926.
 */

let crcTable: Uint32Array | null = null;

function table(): Uint32Array {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  return crcTable;
}

/**
 * Updates a running CRC. Start with 0 and pass the previous result to
 * continue over more bytes: crc32Update(crc32Update(0, a), b) === crc32(a + b).
 */
export function crc32Update(crc: number, data: Uint8Array): number {
  const t = table();
  let c = (crc ^ 0xffffffff) >>> 0;
  for (let i = 0; i < data.length; i++) c = t[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/** CRC-32 of the whole buffer, as an unsigned 32-bit number. */
export function crc32(data: Uint8Array): number {
  return crc32Update(0, data);
}
