import './styles.css'; 

import "@fontsource/bebas-neue"; // Defaults to 400
import "@fontsource/poppins";    // Defaults to 400
import "@fontsource/jetbrains-mono/400.css";
import "@fontsource/jetbrains-mono/700.css";
import "@fontsource/space-mono/400.css";
import "@fontsource/space-mono/700.css";

import "bootstrap-icons/font/bootstrap-icons.css";

import Prism from 'prismjs';
import 'prismjs/themes/prism-okaidia.css';
import 'prismjs/components/prism-lua';

import { WASI, ConsoleStdout, File, OpenFile } from "@bjorn3/browser_wasi_shim";
window.WASI_Shim = { WASI, File, OpenFile, ConsoleStdout };

import Alpine from 'alpinejs';
window.Alpine = Alpine;

import { DiluviumTerminal, InitPrism } from './diluvium_terminal.js';

InitPrism();
Alpine.data('DiluviumTerminal', DiluviumTerminal);
Alpine.start();