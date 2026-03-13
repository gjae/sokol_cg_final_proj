#!/bin/bash
./build/proyecto_final_cg &
PID=$!
sleep 2
kill $PID
