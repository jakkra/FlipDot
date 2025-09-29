import { Badge, Box, Divider, HStack, Stack, Text, VStack } from '@chakra-ui/react';
import { useMemo } from 'react';
import { useConnectionStore } from '../../state/connectionStore.js';

const eventColor = {
  success: 'green',
  error: 'red',
  info: 'gray',
};

function StatusPanel() {
  const events = useConnectionStore((state) => state.events);
  const lastSentAt = useConnectionStore((state) => state.lastSentAt);
  const isConnected = useConnectionStore((state) => state.isConnected);

  const lastSentLabel = useMemo(() => {
    if (!lastSentAt) {
      return 'No frames sent yet';
    }
    const delta = Math.max(0, Date.now() - lastSentAt);
    if (delta < 1000) {
      return 'Just now';
    }
    if (delta < 60_000) {
      return `${Math.floor(delta / 1000)}s ago`;
    }
    const minutes = Math.floor(delta / 60000);
    return `${minutes}m ago`;
  }, [lastSentAt]);

  return (
    <Box bg="gray.800" borderRadius="lg" p={{ base: 4, md: 6 }} shadow="lg">
      <VStack align="stretch" spacing={4}>
        <Stack direction={{ base: 'column', sm: 'row' }} justify="space-between" align={{ base: 'flex-start', sm: 'center' }}>
          <Text fontWeight="semibold" fontSize="lg">
            Status
          </Text>
          <Badge colorScheme={isConnected ? 'green' : 'gray'}>
            {isConnected ? 'Streaming ready' : 'Not connected'}
          </Badge>
        </Stack>

        <HStack spacing={3} justify="space-between">
          <Text fontSize="sm" color="gray.400">
            Last frame push
          </Text>
          <Text fontSize="sm" color="gray.200">
            {lastSentLabel}
          </Text>
        </HStack>

        <Divider borderColor="gray.700" />

        <VStack align="stretch" spacing={2}>
          <Text fontSize="sm" color="gray.400">
            Recent activity
          </Text>
          {events.length === 0 && (
            <Text fontSize="sm" color="gray.500">
              No events yet — connect and start drawing.
            </Text>
          )}
          {events.map((event, index) => (
            <Badge
              key={`${event.message}-${index}`}
              colorScheme={eventColor[event.type] || 'gray'}
              variant="subtle"
              px={3}
              py={1}
              borderRadius="md"
            >
              {event.message}
            </Badge>
          ))}
        </VStack>
      </VStack>
    </Box>
  );
}

export default StatusPanel;
