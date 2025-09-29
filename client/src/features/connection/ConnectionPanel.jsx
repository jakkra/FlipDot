import { useEffect } from 'react';
import {
  Alert,
  AlertDescription,
  AlertIcon,
  AlertTitle,
  Badge,
  Box,
  Button,
  FormControl,
  FormLabel,
  HStack,
  Input,
  Stack,
  Text,
  useBreakpointValue,
  VStack,
} from '@chakra-ui/react';
import { useConnectionStore } from '../../state/connectionStore.js';
import { buildWsUrl } from '../../lib/host.js';

function ConnectionPanel() {
  const host = useConnectionStore((state) => state.host);
  const setHost = useConnectionStore((state) => state.setHost);
  const connect = useConnectionStore((state) => state.connect);
  const disconnect = useConnectionStore((state) => state.disconnect);
  const isConnected = useConnectionStore((state) => state.isConnected);
  const isConnecting = useConnectionStore((state) => state.isConnecting);
  const lastError = useConnectionStore((state) => state.lastError);
  const httpBase = useConnectionStore((state) => state.httpBase);

  const statusColor = isConnected ? 'green' : isConnecting ? 'yellow' : 'gray';
  const statusLabel = isConnected ? 'Connected' : isConnecting ? 'Connecting…' : 'Disconnected';

  const buttonSize = useBreakpointValue({ base: 'md', md: 'sm' });

  useEffect(() => {
    if (!host && typeof window !== 'undefined') {
      const guess = window.location.hostname && window.location.hostname !== 'localhost'
        ? `${window.location.hostname}`
        : '';
      if (guess) {
        setHost(guess);
      }
    }
  }, [host, setHost]);

  return (
    <Box bg="gray.800" borderRadius="lg" p={{ base: 4, md: 6 }} shadow="lg">
      <VStack align="stretch" spacing={4}>
        <Stack direction={{ base: 'column', sm: 'row' }} justify="space-between" align={{ base: 'flex-start', sm: 'center' }}>
          <Text fontWeight="semibold" fontSize="lg">
            Connection
          </Text>
          <Badge colorScheme={statusColor} fontSize="0.85em" px={2} py={1} borderRadius="md">
            {statusLabel}
          </Badge>
        </Stack>

        <FormControl>
          <FormLabel fontSize="sm">Display host / IP</FormLabel>
          <Input
            value={host}
            onChange={(event) => setHost(event.target.value)}
            placeholder="192.168.1.133"
            variant="filled"
            size="md"
            bg="gray.900"
            border="1px solid"
            borderColor="gray.700"
            _focus={{ borderColor: 'brand.500', boxShadow: '0 0 0 1px #fbd55d' }}
            isDisabled={isConnecting}
          />
        </FormControl>

        <HStack spacing={3}>
          <Button
            colorScheme={isConnected ? 'red' : 'yellow'}
            onClick={isConnected ? disconnect : connect}
            isLoading={isConnecting}
            loadingText="Connecting"
            size={buttonSize}
            w={{ base: 'full', sm: 'auto' }}
          >
            {isConnected ? 'Disconnect' : 'Connect'}
          </Button>
          <Text fontSize="sm" color="gray.400">
            WebSocket endpoint: {host ? buildWsUrl(host) : '—'}
          </Text>
        </HStack>

        {httpBase && (
          <Text fontSize="sm" color="gray.500">
            HTTP commands will target <Text as="span" color="gray.200">{httpBase}</Text>
          </Text>
        )}

        {lastError && (
          <Alert status="error" variant="left-accent" borderRadius="md">
            <AlertIcon />
            <Box>
              <AlertTitle fontSize="sm">WebSocket error</AlertTitle>
              <AlertDescription fontSize="sm">{lastError}</AlertDescription>
            </Box>
          </Alert>
        )}
      </VStack>
    </Box>
  );
}

export default ConnectionPanel;
