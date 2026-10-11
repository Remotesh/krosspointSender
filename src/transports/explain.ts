/**
 * Plain-word explanation of a failed transfer, with the fallback list.
 */
import type { TransferError, TransferMethod } from '../types';

export const FALLBACKS = {
  usb: 'Write to a USB folder: put the reader in USB Drive mode, connect the cable, and use "Write to USB folder" (Chrome or Edge).',
  download: 'Download the file and copy it to the reader yourself, or upload it on the CrossPoint web page.',
  localhost: 'Save an offline copy of this page and open it from your disk (or serve it at http://localhost). A page that is not https can talk to the device.',
  helper: 'Run the desktop app. It sends on its own, and it acts as the helper for this page (Settings > Helper URL).',
  wifi: 'Send over Wi-Fi instead: on the reader open File Transfer and use the CrossPoint HTTP upload or WebDAV method. It is much faster than Bluetooth.',
  bleScreen: 'On the reader open File Transfer > Receive via Bluetooth, keep the screen open, then press Connect and send again.',
  cors: 'Use a local endpoint that allows this page: the helper app, or a device or proxy that sends CORS headers for this origin.',
} as const;

function isHttpsOrigin(origin: string): boolean {
  return origin.toLowerCase().startsWith('https:');
}

function isLocalOrigin(origin: string): boolean {
  return /^https?:\/\/(localhost|127\.\d+\.\d+\.\d+|\[::1\])(:\d+)?/i.test(origin);
}

type Explained = { title: string; reasons: string[]; fallbacks: string[] };

/** Bluetooth (method 'ble') failures. The Wi-Fi texts (mixed content, CORS) do not apply. */
function explainBle(err: TransferError): Explained {
  const others = [FALLBACKS.wifi, FALLBACKS.usb, FALLBACKS.download];
  switch (err.code) {
    case 'too-large':
      return {
        title: 'The file is too large for Bluetooth',
        reasons: [err.message, 'Bluetooth sends about 10 to 50 KB/s. The limit is 16 MB.'],
        fallbacks: others,
      };
    case 'not-paired':
      return {
        title: 'Choose the reader first',
        reasons: [err.message],
        fallbacks: [FALLBACKS.bleScreen, ...others],
      };
    case 'unsupported':
      return {
        title: 'This browser cannot use Bluetooth',
        reasons: [err.message, 'Web Bluetooth is in Chrome and Edge (desktop and Android). Firefox and Safari do not have it.'],
        fallbacks: others,
      };
    case 'cancelled':
      return {
        title: 'The transfer was cancelled',
        reasons: [err.message || 'You cancelled the transfer or closed the Bluetooth chooser. Nothing was changed.'],
        fallbacks: [FALLBACKS.bleScreen, FALLBACKS.download],
      };
    case 'device':
      return {
        title: 'The reader reported an error',
        reasons: [err.message, 'The partial file was deleted on the reader. Check the free space on the SD card, then send again.'],
        fallbacks: [FALLBACKS.bleScreen, ...others],
      };
    default:
      return {
        title: 'The Bluetooth transfer failed',
        reasons: [
          err.message || 'An unknown error occurred.',
          'The reader must show File Transfer > Receive via Bluetooth and run the CrossPoint ble-transfer firmware build.',
          'Keep the reader near this computer. Only one computer can connect at a time.',
        ],
        fallbacks: [FALLBACKS.bleScreen, ...others],
      };
  }
}

/**
 * Explains why a transfer was blocked or failed.
 * origin is the page origin, e.g. location.origin.
 * method, when given, picks method-specific text (Bluetooth has its own).
 */
export function explainBlock(err: TransferError, origin: string, method?: TransferMethod): Explained {
  if (method === 'ble' || err.code === 'not-paired') return explainBle(err);
  const https = isHttpsOrigin(origin) && !isLocalOrigin(origin);
  const all = [FALLBACKS.usb, FALLBACKS.download, FALLBACKS.localhost, FALLBACKS.helper, FALLBACKS.cors];
  const pna =
    'Chrome Private Network Access can block a public website from talking to a device on your home network, or ask you first.';
  const mixed = `This page is loaded over https (${origin}). The device uses plain http. Browsers block this as "mixed content".`;

  switch (err.code) {
    case 'mixed-content':
      return {
        title: 'The browser blocked the connection to the device',
        reasons: [mixed, 'The device cannot use https, so this block cannot be turned off from the page.', pna],
        fallbacks: all,
      };
    case 'network': {
      const reasons = [
        'The device did not answer, or the browser stopped the request.',
        'Check that the reader shows the File Transfer screen and is on the same Wi-Fi as this computer.',
        'Check the host name or IP address. If crosspoint.local does not work, use the IP address shown on the reader.',
        'The device or server may not allow requests from this page (CORS).',
      ];
      if (https) reasons.push(mixed);
      reasons.push(pna);
      return { title: 'Could not reach the device', reasons, fallbacks: all };
    }
    case 'not-found':
      return {
        title: 'The device or the file location was not found',
        reasons: [
          'The device did not answer in time, or it returned "not found" (404).',
          'Check the address and the port (80 for CrossPoint). Wake the reader and open the File Transfer screen.',
          'Check the target folder. It must start with "/".',
          ...(https ? [mixed] : []),
        ],
        fallbacks: all,
      };
    case 'auth':
      return {
        title: 'The device or the browser refused access',
        reasons: [
          'The device asked for a username and password (401/403), or the browser did not give folder permission.',
          'Check the username and password in the device profile. CrossPoint needs none.',
        ],
        fallbacks: [FALLBACKS.usb, FALLBACKS.download, FALLBACKS.helper],
      };
    case 'exists':
      return {
        title: 'The file already exists on the device',
        reasons: [
          'CrossPoint does not replace a file with the same name through this upload.',
          'Change the file name, delete the old file on the reader, or use WebDAV (it overwrites).',
        ],
        fallbacks: [FALLBACKS.download, FALLBACKS.usb],
      };
    case 'unsupported':
      return {
        title: 'This browser cannot do this transfer',
        reasons: [
          err.message,
          'USB folder write needs the File System Access API (Chrome or Edge on a desktop). Bluetooth needs Web Bluetooth.',
        ],
        fallbacks: [FALLBACKS.download, FALLBACKS.localhost, FALLBACKS.helper],
      };
    case 'too-large':
      return {
        title: 'The file is too large for this method',
        reasons: [err.message],
        fallbacks: [FALLBACKS.wifi, FALLBACKS.usb, FALLBACKS.download],
      };
    case 'cancelled':
      return {
        title: 'The transfer was cancelled',
        reasons: ['You cancelled the transfer or closed the picker. Nothing was changed.'],
        fallbacks: [FALLBACKS.download],
      };
    case 'device':
      return {
        title: 'The device reported an error',
        reasons: [
          err.message,
          'Check that the SD card has free space and is not read-only. Then send the file again.',
        ],
        fallbacks: [FALLBACKS.usb, FALLBACKS.download, FALLBACKS.helper],
      };
    case 'unknown':
    default:
      return {
        title: 'The transfer failed',
        reasons: [err.message || 'An unknown error occurred.', ...(https ? [mixed, pna] : [])],
        fallbacks: all,
      };
  }
}
