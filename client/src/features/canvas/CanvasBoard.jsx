import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Box, Button, ButtonGroup, HStack, Icon, Stack, Text, Tooltip, VStack } from '@chakra-ui/react';
import { FaEraser, FaPaintBrush, FaTrash } from 'react-icons/fa';
import { useConnectionStore } from '../../state/connectionStore.js';
import { DOT_COLUMNS, DOT_ROWS } from '../../lib/constants.js';

const DOT_SIZE = 26;
const CANVAS_WIDTH = DOT_COLUMNS * DOT_SIZE;
const CANVAS_HEIGHT = DOT_ROWS * DOT_SIZE;

const TOOL_DRAW = 'draw';
const TOOL_ERASE = 'erase';

const getPixelIndex = (col, row) => row * DOT_COLUMNS + col;

function CanvasBoard() {
  const canvasRef = useRef(null);
  const lastPointerIndex = useRef(-1);
  const drawingValue = useRef(255);
  const isDrawing = useRef(false);

  const framebuffer = useConnectionStore((state) => state.framebuffer);
  const updatePixel = useConnectionStore((state) => state.updatePixel);
  const clearFramebuffer = useConnectionStore((state) => state.clearFramebuffer);
  const fillFramebuffer = useConnectionStore((state) => state.fillFramebuffer);
  const sendFramebuffer = useConnectionStore((state) => state.sendFramebuffer);
  const isConnected = useConnectionStore((state) => state.isConnected);

  const [tool, setTool] = useState(TOOL_DRAW);

  const backgroundShade = '#0a1120';
  const dotOffColor = '#1f2937';
  const dotOnColor = '#f9cc2f';

  const applyPixel = useCallback(
    (col, row, value) => {
      if (col < 0 || row < 0 || col >= DOT_COLUMNS || row >= DOT_ROWS) {
        return;
      }
      const index = getPixelIndex(col, row);
      if (index === lastPointerIndex.current && framebuffer[index] === value) {
        return;
      }
      lastPointerIndex.current = index;
      updatePixel(index, value);
    },
    [framebuffer, updatePixel],
  );

  const drawFrame = useCallback(() => {
    const canvas = canvasRef.current;
    if (!canvas) {
      return;
    }
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = backgroundShade;
    ctx.fillRect(0, 0, CANVAS_WIDTH, CANVAS_HEIGHT);

    const radius = DOT_SIZE * 0.42;

    for (let row = 0; row < DOT_ROWS; row += 1) {
      for (let col = 0; col < DOT_COLUMNS; col += 1) {
        const index = getPixelIndex(col, row);
        const isOn = Boolean(framebuffer[index]);
        const cx = col * DOT_SIZE + DOT_SIZE / 2;
        const cy = row * DOT_SIZE + DOT_SIZE / 2;
        ctx.beginPath();
        ctx.fillStyle = isOn ? dotOnColor : dotOffColor;
        ctx.arc(cx, cy, radius, 0, Math.PI * 2);
        ctx.fill();
      }
    }
  }, [backgroundShade, framebuffer, dotOffColor, dotOnColor]);

  useEffect(() => {
    drawFrame();
  }, [drawFrame]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) {
      return;
    }
    canvas.width = CANVAS_WIDTH;
    canvas.height = CANVAS_HEIGHT;
  }, []);

  const canvasContainerSizes = useMemo(
    () => ({
      maxW: '100%',
    }),
    [],
  );

  const resolveCoordinates = useCallback((event) => {
    const canvas = canvasRef.current;
    if (!canvas) {
      return null;
    }

    const rect = canvas.getBoundingClientRect();
    const scaleX = canvas.width / rect.width;
    const scaleY = canvas.height / rect.height;
    const x = (event.clientX - rect.left) * scaleX;
    const y = (event.clientY - rect.top) * scaleY;
    const col = Math.floor(x / DOT_SIZE);
    const row = Math.floor(y / DOT_SIZE);

    if (Number.isNaN(col) || Number.isNaN(row)) {
      return null;
    }

    return { col, row };
  }, []);

  const handlePointerDown = useCallback(
    (event) => {
      event.preventDefault();
      const coords = resolveCoordinates(event);
      if (!coords) {
        return;
      }

      isDrawing.current = true;
      const isEraseAction = event.button === 2 || tool === TOOL_ERASE;
      drawingValue.current = isEraseAction ? 0 : 255;
      applyPixel(coords.col, coords.row, drawingValue.current);

      event.currentTarget.setPointerCapture?.(event.pointerId);
    },
    [applyPixel, resolveCoordinates, tool],
  );

  const handlePointerMove = useCallback(
    (event) => {
      if (!isDrawing.current || event.buttons === 0) {
        return;
      }
      const coords = resolveCoordinates(event);
      if (!coords) {
        return;
      }
      applyPixel(coords.col, coords.row, drawingValue.current);
    },
    [applyPixel, resolveCoordinates],
  );

  const endStroke = useCallback(() => {
    isDrawing.current = false;
    lastPointerIndex.current = -1;
    drawingValue.current = tool === TOOL_ERASE ? 0 : 255;
  }, [tool]);

  const handlePointerUp = useCallback(
    (event) => {
      endStroke();
      event.currentTarget.releasePointerCapture?.(event.pointerId);
      sendFramebuffer();
    },
    [endStroke, sendFramebuffer],
  );

  const handlePointerLeave = useCallback(() => {
    if (!isDrawing.current) {
      return;
    }
    endStroke();
    sendFramebuffer();
  }, [endStroke, sendFramebuffer]);

  const onClear = useCallback(() => {
    clearFramebuffer();
    sendFramebuffer();
  }, [clearFramebuffer, sendFramebuffer]);

  const onFill = useCallback(() => {
    fillFramebuffer();
    sendFramebuffer();
  }, [fillFramebuffer, sendFramebuffer]);

  return (
    <VStack
      spacing={4}
      flex={{ base: 'none', lg: '1' }}
      align="stretch"
      bg="gray.800"
      borderRadius="lg"
      px={{ base: 3, md: 5 }}
      pt={{ base: 3, md: 5 }}
      pb={{ base: 2, md: 4 }}
      shadow="lg"
    >
      <Text fontSize="sm" color="gray.300">
        Tap or drag to light up dots. Switch tools to erase.
      </Text>

      <Box
        borderRadius="lg"
        overflow="hidden"
        bg="gray.900"
        border="1px solid"
        borderColor="gray.700"
        px={{ base: 1, md: 4 }}
        py={{ base: 1, md: 4 }}
        display="flex"
        justifyContent="center"
      >
        <Box
          {...canvasContainerSizes}
          w="full"
          maxH="100%"
          cursor={tool === TOOL_ERASE ? 'none' : 'pointer'}
        >
          <canvas
            ref={canvasRef}
            style={{ width: '100%', height: 'auto', touchAction: 'none' }}
            onPointerDown={handlePointerDown}
            onPointerMove={handlePointerMove}
            onPointerUp={handlePointerUp}
            onPointerLeave={handlePointerLeave}
            onContextMenu={(event) => event.preventDefault()}
          />
        </Box>
      </Box>

      <Stack direction={{ base: 'column', md: 'row' }} spacing={4} align={{ base: 'stretch', md: 'center' }}>
        <ButtonGroup size="sm" isAttached variant="outline" colorScheme="yellow">
          <Button
            onClick={() => setTool(TOOL_DRAW)}
            leftIcon={<Icon as={FaPaintBrush} />}
            colorScheme={tool === TOOL_DRAW ? 'yellow' : undefined}
            variant={tool === TOOL_DRAW ? 'solid' : 'outline'}
          >
            Draw
          </Button>
          <Button
            onClick={() => setTool(TOOL_ERASE)}
            leftIcon={<Icon as={FaEraser} />}
            colorScheme={tool === TOOL_ERASE ? 'yellow' : undefined}
            variant={tool === TOOL_ERASE ? 'solid' : 'outline'}
          >
            Erase
          </Button>
        </ButtonGroup>

        <HStack spacing={3} justify="flex-start">
          <Tooltip label="Clear all pixels" placement="top">
            <Button onClick={onClear} leftIcon={<Icon as={FaTrash} />} size="sm" colorScheme="red" variant="outline">
              Clear
            </Button>
          </Tooltip>
          <Tooltip label="Light all pixels" placement="top">
            <Button onClick={onFill} size="sm" colorScheme="yellow" variant="outline">
              Fill
            </Button>
          </Tooltip>
        </HStack>
      </Stack>

      <Stack direction={{ base: 'column', md: 'row' }} align={{ base: 'flex-start', md: 'center' }} spacing={4}>
        <Button size="sm" onClick={sendFramebuffer} colorScheme="brand" isDisabled={!isConnected}>
          Send Frame Now
        </Button>
      </Stack>
    </VStack>
  );
}

export default CanvasBoard;
