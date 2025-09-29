const normalizeHost = (host) => host.trim().replace(/\/+$/, '');

export const buildHttpBase = (host) => {
  if (!host) {
    return '';
  }

  const trimmed = normalizeHost(host);

  if (trimmed.startsWith('http://') || trimmed.startsWith('https://')) {
    return trimmed;
  }

  if (trimmed.startsWith('ws://') || trimmed.startsWith('wss://')) {
    return trimmed.replace(/^ws/i, 'http');
  }

  return `http://${trimmed}`;
};

export const buildWsUrl = (host) => {
  if (!host) {
    return '';
  }

  const trimmed = normalizeHost(host);

  if (trimmed.startsWith('ws://') || trimmed.startsWith('wss://')) {
    return trimmed.includes('/ws') ? trimmed : `${trimmed}/ws`;
  }

  const withoutProtocol = trimmed.replace(/^https?:\/\//i, '');
  return `ws://${withoutProtocol.includes('/ws') ? withoutProtocol : `${withoutProtocol}/ws`}`;
};
