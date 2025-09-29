import { Box, Container, Stack, VStack } from '@chakra-ui/react';
import CanvasBoard from './features/canvas/CanvasBoard.jsx';
import ConnectionPanel from './features/connection/ConnectionPanel.jsx';
import ModePanel from './features/mode/ModePanel.jsx';
import StatusPanel from './features/status/StatusPanel.jsx';

function App() {
  return (
    <Box minH="100vh" bg="gray.900">
      <Container maxW="6xl" py={{ base: 4, md: 8 }} px={{ base: 0, md: 6 }}>
        <Stack direction={{ base: 'column', lg: 'row' }} spacing={{ base: 6, lg: 10 }} align="stretch">
          <CanvasBoard />

          <VStack flex="1" spacing={6} align="stretch">
            <ConnectionPanel />
            <ModePanel />
            <StatusPanel />
          </VStack>
        </Stack>
      </Container>
    </Box>
  );
}

export default App;
