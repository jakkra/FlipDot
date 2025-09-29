import { extendTheme } from '@chakra-ui/react';

const theme = extendTheme({
  config: {
    initialColorMode: 'dark',
    useSystemColorMode: false,
  },
  styles: {
    global: {
      body: {
        bg: '#0f172a',
        color: 'gray.100',
      },
    },
  },
  colors: {
    brand: {
      400: '#fbd55d',
      500: '#f9cc2f',
      600: '#d9ad0a',
    },
  },
});

export default theme;
