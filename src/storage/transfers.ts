/**
 * Transfer log for sends that have no library entry (store "transfers").
 * "Send a file" writes here with kind 'file'. Article sends stay in
 * LibraryEntry.transfers.
 */
import type { TransferRecord } from '../types';
import { getDb } from './db';

/** Adds one record to the transfer log. */
export async function logTransfer(rec: TransferRecord): Promise<void> {
  if (!rec?.id) throw new Error('The transfer record has no id.');
  const db = await getDb();
  await db.put('transfers', rec);
}

/** Returns the transfer log, newest first. A limit keeps only the newest records. */
export async function listTransferLog(limit?: number): Promise<TransferRecord[]> {
  const db = await getDb();
  const all = (await db.getAllFromIndex('transfers', 'at')).reverse();
  return limit && limit > 0 ? all.slice(0, limit) : all;
}

/** Deletes the whole transfer log. */
export async function clearTransferLog(): Promise<void> {
  const db = await getDb();
  await db.clear('transfers');
}
