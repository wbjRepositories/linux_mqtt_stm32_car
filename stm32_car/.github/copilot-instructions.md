# Copilot Instructions for STM32 MQTT Car Project

## Project Overview
This project is firmware for an STM32F103-based car, integrating MQTT communication, FreeRTOS, and various hardware peripherals. The codebase is organized for embedded C development, targeting Keil MDK-ARM toolchain.

## Architecture & Major Components
- **Core/Inc & Core/Src**: Main application code. Each peripheral (ADC, encoder, I2C, SPI, OLED, USART, etc.) has its own header and source file.
- **Drivers/**: Contains CMSIS and STM32 HAL drivers. Do not modify unless updating vendor code.
- **MDK-ARM/**: Keil project files and build artifacts. Use Keil uVision for building and debugging.
- **Middlewares/Third_Party/FreeRTOS/**: FreeRTOS kernel and configuration.

## Key Patterns & Conventions
- **Peripheral Abstraction**: Each hardware feature is abstracted in its own module (e.g., `adc.c/h`, `encoder.c/h`). Initialization and ISR code are in `main.c` and `stm32f1xx_it.c`.
- **MQTT Task**: MQTT communication logic is in `task_mqtt.c/h`. It interacts with other modules via FreeRTOS queues/events.
- **Configuration**: Use the `.ioc` file for CubeMX-based configuration. Regenerate code only if updating hardware setup.
- **Naming**: File and symbol names follow lower_snake_case for C files, UPPER_SNAKE_CASE for macros.

## Developer Workflows
- **Build**: Open `stm32_car.uvprojx` in Keil uVision. Build using the IDE. Artifacts are in `MDK-ARM/stm32_car/`.
- **Debug**: Use Keil debugger with the provided `.dbgconf` files.
- **Update HAL/FreeRTOS**: Replace files in `Drivers/STM32F1xx_HAL_Driver` or `Middlewares/Third_Party/FreeRTOS` as needed.
- **CubeMX**: Edit `stm32_car.ioc` and regenerate if hardware changes. Manual code in `Core/Src` may be overwritten.

## Integration Points
- **MQTT**: The car communicates with external systems via MQTT. All MQTT logic is in `task_mqtt.c/h`.
- **FreeRTOS**: Tasks, queues, and synchronization are managed via FreeRTOS. See `FreeRTOSConfig.h` and related kernel files.
- **Hardware Drivers**: HAL drivers are in `Drivers/STM32F1xx_HAL_Driver/`. Do not edit unless updating vendor code.

## Examples
- To add a new sensor, create `sensor.c/h` in `Core/Src` and `Core/Inc`, update initialization in `main.c`, and handle interrupts in `stm32f1xx_it.c`.
- To extend MQTT logic, modify `task_mqtt.c/h` and ensure thread safety via FreeRTOS primitives.

## Special Notes
- **Manual Edits**: Code in `Core/Src` may be overwritten by CubeMX. Place custom logic in user code sections (`/* USER CODE BEGIN */ ... /* USER CODE END */`).
- **Build Artifacts**: Ignore files in `MDK-ARM/stm32_car/` for source control.

---
For questions or unclear conventions, ask the user for clarification or examples from their workflow.
