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

const REGISTRY: Record<TransferMethod, TransportAdapter> = {
  webdav: webdavTransport,
  'crosspoint-http': crosspointHttpTransport,
  'crosspoint-ws': crosspointWsTransport,
  usb: usbTransport,
  download: downloadTransport,
  opds: opdsTransport,
  ble: bleMockTransport,
};

/** The adapter for a method. The same object every call. */
export function getTransport(method: TransferMethod): TransportAdapter {
  const t = REGISTRY[method];
  if (!t) throw new Error(`Unknown transfer method: ${String(method)}`);
  return t;
}

/** All adapters, one per TransferMethod. The helper adapter is separate (getHelperTransport). */
export function listTransports(): TransportAdapter[] {
  return Object.values(REGISTRY);
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
export { BleMockTransport, bleMockTransport, crc32, BLE_SERVICE_UUID, BLE_CHUNK_SIZE } from './bleMock';
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
