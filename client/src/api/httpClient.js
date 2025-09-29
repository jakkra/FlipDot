import { buildHttpBase } from '../lib/host.js';

export async function sendGetCommand(host, path) {
  const base = buildHttpBase(host);

  if (!base) {
    throw new Error('Display host is not set');
  }

  const url = `${base}${path}`;
  try {
    const response = await fetch(url, { method: 'GET', mode: 'no-cors' });
    return response;
  } catch (error) {
    throw new Error(error.message || 'Failed to reach display');
  }
}
