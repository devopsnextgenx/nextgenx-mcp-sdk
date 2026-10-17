#!/bin/bash

echo "Starting build process..."

cmake --build build --parallel 4 

echo "starting MCP server"