#!/bin/bash

RED='\033[1;31m'
GREEN='\033[1;32m'
BLUE='\033[1;34m'
NC='\033[0m'

OPT_INSTALL=false
OPT_UNINSTALL=false

help() {
    echo -e "${BLUE}ChromaLeon preload installer${NC}"
    echo ""
    echo "Options:"
    echo "  -i, --install        Compile and install the preload library"
    echo "  -u, --uninstall      Remove the preload library and clean installed files"
    echo "  -h, --help           Show this help menu and exit"
    echo ""
    echo "Example: ./manage.sh -i"
    exit 0
}

if [ "$#" -eq 0 ]; then
    sleep 0.2
    clear

    echo -e "${GREEN}No arguments provided. Starting interactive mode...${NC}"
    echo "Answer with 'y' for yes or press Enter to skip (no)."
    echo "------------------------------------------------------------"

    read -p " -> Install preload library? (y/N): " resp
    if [[ "$resp" =~ ^[SsYy]$ ]]; then
        OPT_INSTALL=true
    else
        read -p " -> Uninstall preload library? (y/N): " resp
        [[ "$resp" =~ ^[SsYy]$ ]] && OPT_UNINSTALL=true
    fi

    echo -e "------------------------------------------------------------\n"
    
    if [ "$OPT_INSTALL" = false ] && [ "$OPT_UNINSTALL" = false ]; then
        echo "No action selected. Exiting."
        exit 0
    fi
else
    while [[ "$#" -gt 0 ]]; do
        case $1 in
            -i|--install) OPT_INSTALL=true ;;
            -u|--uninstall) OPT_UNINSTALL=true ;;
            -h|--help) help ;;
            *) echo -e "${RED}Error: Unknown parameter: $1${NC}"; exit 1 ;;
        esac
        shift
    done
fi

if [ "$OPT_INSTALL" = true ]; then
    echo -e "\n${BLUE}Installing preload library...${NC}"

    if [ -f "chromaleon-preload.c" ]; then
        echo -e "${GREEN}-> chromaleon-preload.c found. Compiling...${NC}"
        
        if gcc -O3 -fPIC -shared chromaleon-preload.c -o libchromaleon.so $(pkg-config --cflags --libs gio-2.0 glib-2.0) -ldl; then
            echo -e "${GREEN}-> libchromaleon.so compiled. Installing...${NC}"
            mkdir -p "$HOME/.local/lib"
            mv libchromaleon.so "$HOME/.local/lib/"

            mkdir -p "$HOME/.config/environment.d"
            cat << 'EOF' > "$HOME/.config/environment.d/chromaleon.conf"
LD_PRELOAD=${HOME}/.local/lib/libchromaleon.so
EOF

            echo -e "${GREEN}-> ChromaLeon preload library installed and activated successfully!${NC}"
            echo -e "${BLUE}\nSession restart required to apply changes.${NC}"
        else
            echo -e "${RED}-> Compilation failed.${NC}"
            exit 1
        fi
    else
        echo -e "${RED}-> chromaleon-preload.c not found. Skipping compilation.${NC}"
        exit 1
    fi
fi

if [ "$OPT_UNINSTALL" = true ]; then
    echo -e "\n${BLUE}Uninstalling preload library...${NC}"

    rm -f "$HOME/.local/lib64/libchromaleon.so"
    rm -f "$HOME/.local/lib32/libchromaleon.so"
    rm -f "$HOME/.local/lib/libchromaleon.so"
    rm -f "$HOME/.config/environment.d/chromaleon.conf"

    echo -e "${GREEN}-> ChromaLeon preload uninstalled and environment cleaned.${NC}"
fi