import wasmUrl from './libdiluvium';

export function InitPrism()
{
    Prism.languages.insertBefore('lua', 'operator', {
        'null-coalescing': {
            pattern: /\?\?/,
            alias: 'operator'
        }
    });

    Prism.languages.insertBefore('lua', 'string', {
        'interpolated-string': {
            pattern: /\$"(?:[^"\\{]|\\[\s\S]|\{(?:[^{}"]|"(?:[^"\\\r\n]|\\.)*")*\})*"/,
            greedy: true,
            inside: {
                'interpolation': {
                    pattern: /\{(?:[^{}"]|"(?:[^"\\\r\n]|\\.)*")*\}/,
                    inside: {
                        'interpolation-punctuation': {
                            pattern: /^\{|\}$/,
                            alias: 'punctuation'
                        },
                        'interpolation-content': {
                            pattern: /[a-zA-Z_]\w*/,
                            alias: 'variable'
                        },
                        rest: Prism.languages.lua
                    }
                },
                'string': /[\s\S]+/
            }
        }
    });
}

export function DiluviumTerminal()
{
    return {
        history: [
            { type: 'system', text: 'Initializing environment...' }
        ],
        cmdHistory: [],
        isProcessing: false,
        historyIndex: -1,
        currentInput: '',
        status: 'Booting...',
        wasi: null,
        exports: null,
        filteredSuggestions: [],
        suggestionIndex: 0,
        // A basic list of Lua 5.4 keywords + globals + your custom ones
        knownWords: [
            'and', 'break', 'do', 'else', 'elseif', 'end', 'false', 'for', 'function',
            'goto', 'if', 'in', 'local', 'nil', 'not', 'or', 'repeat', 'return',
            'then', 'true', 'until', 'while',
            'print', 'pairs', 'ipairs', 'tonumber', 'tostring', 'type',
            'math', 'string', 'table', 'io', 'os', 'coroutine', 'package', 'utf8',
        ],
        async initWasm()
        {
            const term = this;
            try
            {
                // Wait for ES Module to load
                while (!window.WASI_Shim) await new Promise(r => setTimeout(r, 50));
                const { WASI, File, OpenFile, ConsoleStdout } = window.WASI_Shim;

                // Setup Standard Output/Error with buffering
                const stdout = ConsoleStdout.lineBuffered(msg =>
                {


                    if (msg.startsWith('@@KEYS:'))
                    {
                        // 1. Parse the keys (comma separated)
                        const newKeys = msg.substring(7).split(',');

                        // 2. Merge with defaults (Standard Lua keywords)
                        const defaults = [
                            'and', 'break', 'do', 'else', 'elseif', 'end', 'false', 'for',
                            'function', 'goto', 'if', 'in', 'local', 'nil', 'not', 'or',
                            'repeat', 'return', 'then', 'true', 'until', 'while'
                        ];

                        // 3. Update the list (Set removes duplicates)
                        term.knownWords = [...new Set([...defaults, ...newKeys])];
                        return; // STOP: Don't print this to the terminal
                    }
                    term.pushHistory('output', msg);

                });

                const stderr = ConsoleStdout.lineBuffered(msg =>
                {
                    term.pushHistory('error', msg);
                });

                // Configure WASI
                this.wasi = new WASI(
                    ["lua"],                    // args
                    [],                         // env
                    [                           // fds
                        new OpenFile(new File([])), // stdin
                        stdout,                     // stdout
                        stderr                      // stderr
                    ]
                );

                // Load Binary
                // const response = await fetch('libdiluvium.wasm');
                const response = await fetch(wasmUrl);
                const buffer = await response.arrayBuffer();

                const { instance } = await WebAssembly.instantiate(buffer, {
                    "wasi_snapshot_preview1": this.wasi.wasiImport
                });

                this.exports = instance.exports;
                this.wasi.start(instance);

                // Initialize Lua
                if (this.exports.__wasm_call_ctors) this.exports.__wasm_call_ctors();
                if (this.exports.init_lua) this.exports.init_lua();


                const setupScript = `
                    function _G.__dump_keys()
                        local keys = {}
                        for k,v in pairs(_G) do
                            if type(k) == "string" then table.insert(keys, k) end
                        end
                        -- Print with our secret prefix
                        print("@@KEYS:" .. table.concat(keys, ","))
                    end
                `;
                this.runLuaString(setupScript);

                this.status = 'Ready';

                this.pushHistory('system', "[A note on web REPL usage]: Variables defined with local are scoped to the current execution block only. To create persistent variables across commands, remove local.")
                this.pushHistory('system', '---');
                this.pushHistory('system', 'Type "clear" to clear screen. Use `ctrl-r` to reset runtime.');

                // Load history from storage if it exists
                const saved = localStorage.getItem('lua_history');
                if (saved)
                {
                    this.cmdHistory = JSON.parse(saved);
                    this.historyIndex = this.cmdHistory.length;
                }

            } catch (e)
            {
                console.error(e);
                this.status = 'Error';
                this.pushHistory('error', `CRITICAL: ${e.message}`);
            }
        },

        execute()
        {
            let code = this.currentInput.trim();

            if (!code)
            {
                this.pushHistory('input', ''); // Adds the "➜" line to history
                this.currentInput = '';        // Clears any leftover whitespace
                this.resizeInput();            // Resets height
                return;                        // Stop here (don't run WASM)
            }
            else if (this.currentInput.length > 100000)
            {
                this.pushHistory('error', 'Input too large (max 100KB)');
                return;
            }

            code = code.startsWith('=') ? `print(${code.substring(1)})` : code;

            // Handle built-in CLI commands
            if (code === 'clear' || code === 'cls')
            {
                this.history = [];
                this.currentInput = '';
                return;
            }

            // Push to visual history and command history
            this.pushHistory('input', this.currentInput);
            const lastCmd = this.cmdHistory[this.cmdHistory.length - 1];
            if (this.currentInput !== lastCmd)
            {
                this.cmdHistory.push(this.currentInput);
                if (this.cmdHistory.length > 500)
                    this.cmdHistory.shift();
                localStorage.setItem('lua_history', JSON.stringify(this.cmdHistory));
            }
            this.historyIndex = this.cmdHistory.length;

            if (this.cmdHistory.length > 500)
                this.cmdHistory.shift();
            localStorage.setItem('lua_history', JSON.stringify(this.cmdHistory));

            const codeToRun = code;
            this.currentInput = '';
            this.resizeInput();
            this.status = 'Busy';
            this.isProcessing = true;
            const startTime = performance.now();
            requestAnimationFrame(() =>
            {
                setTimeout(() =>
                {
                    try
                    {
                        const encoder = new TextEncoder();
                        const bytes = encoder.encode(codeToRun + '\0');

                        const ptr = this.exports.malloc(bytes.length);
                        const memory = new Uint8Array(this.exports.memory.buffer);
                        memory.set(bytes, ptr);

                        this.exports.run_lua(ptr);
                        this.exports.free(ptr);

                        this.refreshSuggestions();
                    }
                    catch (e)
                    {
                        this.pushHistory('error', `Runtime Error: ${e.message}`);
                    }
                    finally
                    {
                        this.status = 'Ready';
                        this.isProcessing = false;
                        this.$nextTick(() => this.focusInput());
                        const duration = (performance.now() - startTime).toFixed(2);
                        this.pushHistory('system', `Executed in ${duration}ms`);
                    }
                }, 10);
            });
        },
        resizeInput()
        {
            this.$nextTick(() =>
            {
                const el = this.$refs.cmdInput;
                el.style.height = 'auto'; // Reset to calculate shrink
                el.style.height = el.scrollHeight + 'px'; // Expand to fit
            });
        },
        resetWasm()
        {
            if (!confirm("Are you sure you want to reset the Diluvium environment? All variables will be lost."))
            {
                this.focusInput();
                return;
            }

            // 1. Clear UI
            this.history = [{ type: 'system', text: 'Resetting environment...' }];
            this.currentInput = '';
            this.status = 'Resetting...';

            // 2. Clear Internal State
            this.wasi = null;
            this.exports = null;

            // 3. Re-initialize
            setTimeout(() => this.initWasm(), 100);
        },
        get highlightedInput()
        {
            if (!this.currentInput)
                return '';

            if (this.isProcessing)
            {
                return this.currentInput;
            }
            else
            {
                let html = Prism.highlight(this.currentInput, Prism.languages.lua, 'lua');
                if (this.currentInput.endsWith('\n'))
                    html += '<br>';
                return html;
            }
        },

        pushHistory(type, text)
        {
            const index = this.history.length;

            const sanitized = text
                .replace(/&/g, '&amp;')
                .replace(/</g, '&lt;')
                .replace(/>/g, '&gt;');

            this.history.push({
                type: type,
                text: text,
                html: sanitized
            });

            if (type === 'error')
            {
                const match = text.match(/line (\d+)/);
                if (match)
                {
                    const lineNum = parseInt(match[1]);
                    // TODO: highlight the problematic line in history
                }
            }

            if (type === 'input') 
            {
                const highlighted = Prism.highlight(text, Prism.languages.lua, 'lua');
                this.history[index].html = highlighted;
            }

            this.$nextTick(() => 
            {
                const term = document.getElementById('terminal-window');
                if (term) term.scrollTop = term.scrollHeight;
            });
        },

        historyUp() 
        {
            if (this.historyIndex > 0) 
            {
                this.historyIndex--;
                this.currentInput = this.cmdHistory[this.historyIndex];
            }
        },

        historyDown() 
        {
            if (this.historyIndex < this.cmdHistory.length - 1) 
            {
                this.historyIndex++;
                this.currentInput = this.cmdHistory[this.historyIndex];
            } else
            {
                this.historyIndex = this.cmdHistory.length;
                this.currentInput = '';
            }
        },

        focusInput()
        {
            if (window.getSelection().type !== 'Range')
            {
                this.$refs.cmdInput.focus();
            }
        },
        updateSuggestions()
        {
            const input = this.$refs.cmdInput;
            const text = this.currentInput;
            const cursor = input.selectionStart;
            const textBeforeCursor = text.slice(0, cursor);
            const match = textBeforeCursor.match(/([a-zA-Z_][a-zA-Z0-9_]*)$/);

            if (!match)
            {
                this.filteredSuggestions = [];
                return;
            }

            const currentWord = match[1];

            // Filter knownWords
            if (currentWord.length > 0)
            {
                this.filteredSuggestions = this.knownWords.filter(w =>
                    w.startsWith(currentWord) && w !== currentWord
                ).slice(0, 5);
            }
            else
            {
                this.filteredSuggestions = [];
            }

            this.suggestionIndex = 0;
        },

        acceptSuggestion(word)
        {
            const input = this.$refs.cmdInput;
            const cursor = input.selectionStart;
            const text = this.currentInput;
            const textBeforeCursor = text.slice(0, cursor);

            // Find the partial word we are replacing
            const match = textBeforeCursor.match(/([a-zA-Z_][a-zA-Z0-9_]*)$/);
            if (!match) return;

            const partial = match[1];

            // Construct new string
            const textAfterCursor = text.slice(cursor);
            const newText = textBeforeCursor.slice(0, -partial.length) + word + textAfterCursor;

            // Update state
            this.currentInput = newText;
            this.filteredSuggestions = [];

            // Move cursor to end of inserted word
            this.$nextTick(() =>
            {
                input.selectionStart = input.selectionEnd = (cursor - partial.length) + word.length;
                input.focus();
            });
        },
        runLuaString(code)
        {
            if (!this.exports) return;
            const encoder = new TextEncoder();
            const bytes = encoder.encode(code + '\0');
            const ptr = this.exports.malloc(bytes.length);
            const memory = new Uint8Array(this.exports.memory.buffer);
            memory.set(bytes, ptr);
            this.exports.run_lua(ptr);
            this.exports.free(ptr);
        },
        refreshSuggestions()
        {
            this.runLuaString("_G.__dump_keys()");
        },
        exportSession() 
        {
            const session = {
                history: this.history,
                globals: this.knownWords,
                timestamp: Date.now()
            };
            const blob = new Blob([JSON.stringify(session, null, 2)],
                { type: 'application/json' });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `diluvium-session-${Date.now()}.json`;
            a.click();
            URL.revokeObjectURL(url);
        },
        searchHistory(query) 
        {
            // TODO: Ctrl+F to search
            return this.history.filter(h =>
                h.text.toLowerCase().includes(query.toLowerCase())
            );
        }
    }
}