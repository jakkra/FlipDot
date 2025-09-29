import { sendGetCommand } from './httpClient.js';

export const requestSetMode = (host, mode, text) => {
  const params = new URLSearchParams();
  params.append('mode', String(mode));
  if (text) {
    params.append('text', text);
  }
  return sendGetCommand(host, `/mode?${params.toString()}`);
};

export const requestNextMode = (host) => sendGetCommand(host, '/mode/next');
export const requestPrevMode = (host) => sendGetCommand(host, '/mode/prev');
export const requestInvert = (host, enabled) => {
  if (typeof enabled === 'boolean') {
    return sendGetCommand(host, `/display/invert?enabled=${enabled ? '1' : '0'}`);
  }
  return sendGetCommand(host, '/display/invert');
};
