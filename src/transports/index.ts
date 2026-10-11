/**
 * Transport registry. getTransport returns one singleton adapter per method.
 */
import type { TransferMethod, TransportAdapter } from '../types';
import { webdavTransport } from './webdav';
import { crosspointHttpTransport } from './crosspointHttp';
import { crosspointWsTransport } from './crosspointWs';
import { usbTransport } from './usb';
import { downloadTransport } from './download';
import { opdsTransport } from './opds';
import { bleMockTransport } from './bleMock';
import { bleTransport } from './ble';

const REGISTRY: Record<TransferMethod, TransportAdapter> = {
  webdav: webdavTransport,
  'crosspoint-http': crosspointHttpTransport,
  'crosspoint-ws': crosspointWsTransport,
  usb: usbTransport,
  download: downloadTransport,
  opds: opdsTransport,
  ble: bleTransport,
};

/**
 * Dev flag for the BLE mock: only in a dev build (npm run dev), with ?blemock
 * in the URL or localStorage "krosspoint.bleMock" = "1". Production builds
 * always use the real Web Bluetooth transport.
 */
export function bleMockEnabled(): boolean {
  if (!import.meta.env.DEV) return false;
  try {
    if (new URLSearchParams(globalThis.location?.search ?? '').has('blemock')) return true;
    return globalThis.localStorage?.getItem('krosspoint.bleMock') === '1';
  } catch {
    return false;
  }
}

/** The adapter for a method. The same object every call. */
export function getTransport(method: TransferMethod): TransportAdapter {
  if (method === 'ble' && bleMockEnabled()) return bleMockTransport;
  const t = REGISTRY[method];
  if (!t) throw new Error(`Unknown transfer method: ${String(method)}`);
  return t;
}

/** All adapters, one per TransferMethod. The helper adapter is separate (getHelperTransport). */
export function listTransports(): TransportAdapter[] {
  return Object.values(REGISTRY).map((t) => getTransport(t.method));
}

export { explainBlock, FALLBACKS } from './explain';
export { WebDavTransport, webdavTransport } from './webdav';
export { CrossPointHttpTransport, crosspointHttpTransport } from './crosspointHttp';
export { CrossPointWsTransport, crosspointWsTransport, WS_PORT, WS_CHUNK_SIZE, WS_HIGH_WATER, wsUrl } from './crosspointWs';
export {
  UsbTransport,
  usbTransport,
  pickUsbFolder,
  forgetUsbFolder,
  usbFolderName,
  usbHandleKey,
  isUsbSupported,
  USB_PICKER_ID,
} from './usb';
export { DownloadTransport, downloadTransport, downloadBlob } from './download';
export { OpdsTransport, opdsTransport, buildOpdsCatalog, buildOpdsPackage, xmlEscape } from './opds';
export type { OpdsEntry } from './opds';
export { BleMockTransport, bleMockTransport, BLE_CHUNK_SIZE } from './bleMock';
export { crc32, crc32Update } from './crc32';
export {
  BleTransport,
  bleTransport,
  WebBluetoothLink,
  onBleDeviceChosen,
  forgetBleDevice,
  hasCachedBleDevice,
  BLE_SERVICE_UUID,
  BLE_MAX_BYTES,
  BLE_WARN_BYTES,
} from './ble';
export type { BleLink, BleLinkFactory, BleTransportOptions } from './ble';
export { HelperTransport, getHelperTransport, helperForwardMethod } from './helper';
export {
  baseUrl,
  isMixedContent,
  isLoopbackHost,
  normalizeFolder,
  remoteFilePath,
  toTransferError,
  CONNECT_TIMEOUT_MS,
} from './common';
export { listRemoteFolder, makeRemoteFolder, canBrowseRemote, parsePropfind } from './remoteFolder';
export type { RemoteEntry } from './remoteFolder';
