/**
 * Plain-word explanation of a failed transfer, with the fallback list.
 */
import type { TransferError } from '../types';

export const FALLBACKS = {
  usb: 'Write to a USB folder: put the reader in USB Drive mode, connect the cable, and use "Write to USB folder" (Chrome or Edge).',
  download: 'Download the file and copy it to the reader yourself, or upload it on the CrossPoint web page.',
  localhost: 'Save an offline copy of this page and open it from your disk (or serve it at http://localhost). A page that is not https can talk to the device.',
  helper: 'Run the desktop app. It sends on its own, and it acts as the helper for this page (Settings > Helper URL).',
  cors: 'Use a local endpoint that allows this page: the helper app, or a device or proxy that sends CORS headers for this origin.',
} as const;

function isHttpsOrigin(origin: string): boolean {
  return origin.toLowerCase().startsWith('https:');
}

function isLocalOrigin(origin: string): boolean {
  return /^https?:\/\/(localhost|127\.\d+\.\d+\.\d+|\[::1\])(:\d+)?/i.test(origin);
}

/**
 * Explains why a transfer was blocked or failed.
 * origin is the page origin, e.g. location.origin.
 */
export function explainBlock(
  err: TransferError,
  origin: string,
): { title: string; reasons: string[]; fallbacks: string[] } {
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
