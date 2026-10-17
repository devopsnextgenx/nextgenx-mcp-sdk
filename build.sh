#!/bin/bash

echo "Starting build process..."



# cmake --build build --parallel 4 
cmake --build build --target cppmcp_stdio_client cppmcp_stdio_server
cd build
./cppmcp_stdio_client ./cppmcp_stdio_server

echo "starting MCP server"