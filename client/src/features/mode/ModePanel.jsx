import { useState } from 'react';
import {
  Box,
  Button,
  FormControl,
  FormLabel,
  HStack,
  Input,
  SimpleGrid,
  Stack,
  Switch,
  Text,
  Textarea,
  VStack,
} from '@chakra-ui/react';
import { useMutation } from '@tanstack/react-query';
import { requestInvert, requestNextMode, requestPrevMode, requestSetMode } from '../../api/endpoints.js';
import { useConnectionStore } from '../../state/connectionStore.js';

const MODE_OPTIONS = [
  { id: 0, label: 'Clock Digital' },
  { id: 8, label: 'Clock + Weather' },
  { id: 1, label: 'Scroll Text' },
  { id: 2, label: 'Remote Control' },
  { id: 3, label: 'Solar' },
  { id: 6, label: 'Maintenance' },
  { id: 7, label: 'Clock Analog' },
  { id: 20, label: 'Fireflies' },
  { id: 21, label: 'Cellular Automata' },
  { id: 22, label: 'Matrix Rain' },
  { id: 23, label: 'Ripple' },
  { id: 24, label: 'Tunnel' },
  { id: 25, label: 'Bouncing Balls' },
  { id: 26, label: 'Teleport' },
  { id: 27, label: 'Lissajous' },
];

function ModePanel() {
  const [scrollText, setScrollText] = useState('');
  const [statusMessage, setStatusMessage] = useState('');
  const [statusTone, setStatusTone] = useState('info');

  const host = useConnectionStore((state) => state.host);
  const invert = useConnectionStore((state) => state.invert);
  const setInvert = useConnectionStore((state) => state.setInvert);
  const pushEvent = useConnectionStore((state) => state.pushEvent);

  const toneColor = {
    success: 'green.300',
    error: 'red.300',
    warning: 'yellow.300',
    info: 'gray.300',
  };

  const showStatus = (message, tone = 'info') => {
    setStatusMessage(message);
    setStatusTone(tone);
  };

  const ensureHost = () => {
    if (!host) {
      showStatus('Add the display IP first.', 'warning');
      return false;
    }
    return true;
  };

  const modeMutation = useMutation({
    mutationFn: ({ mode, text }) => requestSetMode(host, mode, text),
    onSuccess: (_, variables) => {
      showStatus(`Mode ${variables.mode} requested.`, 'success');
      pushEvent('info', `Mode ${variables.mode} requested`);
    },
    onError: (error) => {
      showStatus(error.message || 'Failed to reach display.', 'error');
      pushEvent('error', error.message || 'Mode command failed');
    },
  });

  const nextPrevMutation = useMutation({
    mutationFn: (direction) => (direction === 'next' ? requestNextMode(host) : requestPrevMode(host)),
    onSuccess: (_, direction) => {
      const description = direction === 'next' ? 'Requested next mode.' : 'Requested previous mode.';
      showStatus(description, 'info');
      pushEvent('info', description);
    },
    onError: (error) => {
      showStatus(error.message || 'Failed to change mode.', 'error');
      pushEvent('error', error.message || 'Mode change failed');
    },
  });

  const handleSelectMode = (mode) => {
    if (!ensureHost()) {
      return;
    }
    modeMutation.mutate({ mode });
  };

  const handleScrollSubmit = () => {
    if (!ensureHost()) {
      return;
    }
    modeMutation.mutate({ mode: 1, text: scrollText });
  };

  const handleInvertToggle = () => {
    if (!ensureHost()) {
      return;
    }
    const nextInvert = !invert;
    requestInvert(host, nextInvert)
      .then(() => {
        setInvert(nextInvert);
        const message = `Display invert ${nextInvert ? 'enabled' : 'disabled'}.`;
        showStatus(message, 'success');
        pushEvent('info', message);
      })
      .catch((error) => {
        showStatus(error.message || 'Failed to toggle invert.', 'error');
        pushEvent('error', error.message || 'Invert toggle failed');
      });
  };

  return (
    <Box bg="gray.800" borderRadius="lg" p={{ base: 4, md: 6 }} shadow="lg">
      <VStack align="stretch" spacing={5}>
        <Text fontWeight="semibold" fontSize="lg">
          Modes & Commands
        </Text>

        {statusMessage && (
          <Box
            bg="gray.900"
            borderRadius="md"
            px={3}
            py={2}
            borderLeftWidth="4px"
            borderColor={toneColor[statusTone] || 'gray.500'}
          >
            <Text fontSize="sm" color={toneColor[statusTone] || 'gray.300'}>
              {statusMessage}
            </Text>
          </Box>
        )}

        <SimpleGrid columns={{ base: 2, md: 3 }} spacing={3}>
          {MODE_OPTIONS.map((mode) => (
            <Button
              key={mode.id}
              variant="outline"
              colorScheme="yellow"
              size="sm"
              onClick={() => handleSelectMode(mode.id)}
              isLoading={modeMutation.isPending}
            >
              {mode.label}
            </Button>
          ))}
        </SimpleGrid>

        <Stack direction={{ base: 'column', md: 'row' }} spacing={3}>
          <Button
            colorScheme="yellow"
            variant="solid"
            onClick={() => ensureHost() && nextPrevMutation.mutate('prev')}
            isLoading={nextPrevMutation.isPending}
          >
            Previous
          </Button>
          <Button
            colorScheme="yellow"
            variant="solid"
            onClick={() => ensureHost() && nextPrevMutation.mutate('next')}
            isLoading={nextPrevMutation.isPending}
          >
            Next
          </Button>
        </Stack>

        <FormControl>
          <FormLabel fontSize="sm">Scroll text</FormLabel>
          <HStack align="flex-start" spacing={3}>
            <Textarea
              value={scrollText}
              onChange={(event) => setScrollText(event.target.value)}
              placeholder="Message to scroll"
              rows={2}
              bg="gray.900"
              borderColor="gray.700"
              _focus={{ borderColor: 'brand.500' }}
            />
            <Button colorScheme="yellow" onClick={handleScrollSubmit} isLoading={modeMutation.isPending}>
              Send
            </Button>
          </HStack>
        </FormControl>

        <HStack spacing={3}>
          <Switch colorScheme="yellow" isChecked={invert} onChange={handleInvertToggle} />
          <Text fontSize="sm" color="gray.300">
            Invert display
          </Text>
        </HStack>
      </VStack>
    </Box>
  );
}

export default ModePanel;
