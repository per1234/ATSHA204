#include "Arduino.h"
#include "sha204_library.h"
#include "sha204_includes/sha204_lib_return_codes.h"


// atsha204Class Constructor
// Feed this function the Arduino-ized pin number you want to assign to the ATSHA204's SDA pin
// This will find the DDRX, PORTX, and PINX registrs it'll need to point to to control that pin
// As well as the bit value for each of those registers
atsha204Class::atsha204Class(uint8_t pin)
{	
	device_pin = digitalPinToBitMask(pin);	// Find the bit value of the pin
	uint8_t port = digitalPinToPort(pin);	// temoporarily used to get the next three registers
	
	// Point to data direction register port of pin
	device_port_DDR = portModeRegister(port);
	// Point to output register of pin
	device_port_OUT = portOutputRegister(port);
	// Point to input register of pin
	device_port_IN = portInputRegister(port);
}

/* 	Puts a the ATSHA204's unique, 4-byte serial number in the response array 
	returns an SHA204 Return code */
uint8_t atsha204Class::getSerialNumber(uint8_t * response)
{
	uint8_t readCommand[READ_COUNT];
	uint8_t readResponse[READ_4_RSP_SIZE];
	
	/* read from bytes 0->3 of config zone */
	uint8_t returnCode = sha204m_read(readCommand, readResponse, SHA204_ZONE_CONFIG, ADDRESS_SN03);
	if (!returnCode)	// should return 0 if successful
	{
		for (int i=0; i<4; i++)	// store bytes 0-3 into respones array
			response[i] = readResponse[SHA204_BUFFER_POS_DATA+i];
			
		/* read from bytes 8->11 of config zone */
		returnCode = sha204m_read(readCommand, readResponse, SHA204_ZONE_CONFIG, ADDRESS_SN47);
		
		for (int i=4; i<8; i++)	// store bytes 4-7 of SN into response array
			response[i] = readResponse[SHA204_BUFFER_POS_DATA+(i-4)];
		
		if (!returnCode)
		{	/* Finally if last two reads were successful, read byte 8 of the SN */
			returnCode = sha204m_read(readCommand, readResponse, SHA204_ZONE_CONFIG, ADDRESS_SN8);
			response[8] = readResponse[SHA204_BUFFER_POS_DATA];	// Byte 8 of SN should always be 0xEE
		}
	}
	
	return returnCode;
}

/* SWI bit bang functions */

void atsha204Class::swi_set_signal_pin(uint8_t is_high)
{
  *device_port_DDR |= device_pin;

  if (is_high)
    *device_port_OUT |= device_pin;
  else
    *device_port_OUT &= ~device_pin;
}

uint8_t atsha204Class::swi_send_bytes(uint8_t count, uint8_t *buffer)
{
  uint8_t i, bit_mask;

  // Disable interrupts while sending.
  noInterrupts();  //swi_disable_interrupts();

  // Set signal pin as output.
  *device_port_OUT |= device_pin;
  *device_port_DDR |= device_pin;

  // Wait turn around time.
  delayMicroseconds(RX_TX_DELAY);  //RX_TX_DELAY;

  for (i = 0; i < count; i++) 
  {
    for (bit_mask = 1; bit_mask > 0; bit_mask <<= 1) 
    {
      if (bit_mask & buffer[i]) 
      {
        *device_port_OUT &= ~device_pin;
        delayMicroseconds(BIT_DELAY);  //BIT_DELAY_1;
        *device_port_OUT |= device_pin;
        delayMicroseconds(7*BIT_DELAY);  //BIT_DELAY_7;
      }
      else 
      {
        // Send a zero bit.
        *device_port_OUT &= ~device_pin;
        delayMicroseconds(BIT_DELAY);  //BIT_DELAY_1;
        *device_port_OUT |= device_pin;
        delayMicroseconds(BIT_DELAY);  //BIT_DELAY_1;
        *device_port_OUT &= ~device_pin;
        delayMicroseconds(BIT_DELAY);  //BIT_DELAY_1;
        *device_port_OUT |= device_pin;
        delayMicroseconds(5*BIT_DELAY);  //BIT_DELAY_5;
      }
    }
  }
  interrupts();  //swi_enable_interrupts();
  return SWI_FUNCTION_RETCODE_SUCCESS;
}

uint8_t atsha204Class::swi_send_byte(uint8_t value)
{
  return swi_send_bytes(1, &value);
}

uint8_t atsha204Class::swi_receive_bytes(uint8_t count, uint8_t *buffer) 
{
  uint8_t status = SWI_FUNCTION_RETCODE_SUCCESS;
  uint8_t i;
  uint8_t bit_mask;
  uint8_t pulse_count;
  uint8_t timeout_count;

  // Disable interrupts while receiving.
  noInterrupts(); //swi_disable_interrupts();

  // Configure signal pin as input.
  *device_port_DDR &= ~device_pin;

  // Receive bits and store in buffer.
  for (i = 0; i < count; i++)
  {
    for (bit_mask = 1; bit_mask > 0; bit_mask <<= 1) 
    {
      pulse_count = 0;

      // Make sure that the variable below is big enough.
      // Change it to uint16_t if 255 is too small, but be aware that
      // the loop resolution decreases on an 8-bit controller in that case.
      timeout_count = START_PULSE_TIME_OUT;

      // Detect start bit.
      while (--timeout_count > 0) 
      {
        // Wait for falling edge.
        if ((*device_port_IN & device_pin) == 0)
          break;
      }

      if (timeout_count == 0) 
      {
        status = SWI_FUNCTION_RETCODE_TIMEOUT;
        break;
      }

      do 
      {
        // Wait for rising edge.
        if ((*device_port_IN & device_pin) != 0) 
        {
          // For an Atmel microcontroller this might be faster than "pulse_count++".
          pulse_count = 1;
          break;
        }
      } while (--timeout_count > 0);

      if (pulse_count == 0) 
      {
        status = SWI_FUNCTION_RETCODE_TIMEOUT;
        break;
      }

      // Trying to measure the time of start bit and calculating the timeout
      // for zero bit detection is not accurate enough for an 8 MHz 8-bit CPU.
      // So let's just wait the maximum time for the falling edge of a zero bit
      // to arrive after we have detected the rising edge of the start bit.
      timeout_count = ZERO_PULSE_TIME_OUT;

      // Detect possible edge indicating zero bit.
      do 
      {
        if ((*device_port_IN & device_pin) == 0) 
        {
          // For an Atmel microcontroller this might be faster than "pulse_count++".
          pulse_count = 2;
          break;
        }
      } while (--timeout_count > 0);

      // Wait for rising edge of zero pulse before returning. Otherwise we might interpret
      // its rising edge as the next start pulse.
      if (pulse_count == 2) 
      {
        do 
        {
          if ((*device_port_IN & device_pin) != 0)
            break;
        } while (timeout_count-- > 0);
      }

      // Update byte at current buffer index.
      else
        buffer[i] |= bit_mask;  // received "one" bit
    }

    if (status != SWI_FUNCTION_RETCODE_SUCCESS)
      break;
  }
  interrupts(); //swi_enable_interrupts();

  if (status == SWI_FUNCTION_RETCODE_TIMEOUT) 
  {
    if (i > 0)
    // Indicate that we timed out after having received at least one byte.
    status = SWI_FUNCTION_RETCODE_RX_FAIL;
  }
  return status;
}

/* Physical functions */

uint8_t atsha204Class::sha204p_wakeup()
{
  swi_set_signal_pin(0);
  delayMicroseconds(10*SHA204_WAKEUP_PULSE_WIDTH);
  swi_set_signal_pin(1);
  delay(SHA204_WAKEUP_DELAY);

  return SHA204_SUCCESS;
}

uint8_t atsha204Class::sha204p_sleep()
{
  return swi_send_byte(SHA204_SWI_FLAG_SLEEP);
}

uint8_t atsha204Class::sha204p_resync(uint8_t size, uint8_t *response)
{
  delay(SHA204_SYNC_TIMEOUT);
  return sha204p_receive_response(size, response);
}

uint8_t atsha204Class::sha204p_receive_response(uint8_t size, uint8_t *response)
{
  uint8_t count_byte;
  uint8_t i;
  uint8_t ret_code;

  for (i = 0; i < size; i++)
    response[i] = 0;

  (void) swi_send_byte(SHA204_SWI_FLAG_TX);

  ret_code = swi_receive_bytes(size, response);
  if (ret_code == SWI_FUNCTION_RETCODE_SUCCESS || ret_code == SWI_FUNCTION_RETCODE_RX_FAIL) 
  {
    count_byte = response[SHA204_BUFFER_POS_COUNT];
    if ((count_byte < SHA204_RSP_SIZE_MIN) || (count_byte > size))
      return SHA204_INVALID_SIZE;

    return SHA204_SUCCESS;
  }

  // Translate error so that the Communication layer
  // can distinguish between a real error or the
  // device being busy executing a command.
  if (ret_code == SWI_FUNCTION_RETCODE_TIMEOUT)
    return SHA204_RX_NO_RESPONSE;
  else
    return SHA204_RX_FAIL;
}

uint8_t atsha204Class::sha204p_send_command(uint8_t count, uint8_t * command)
{
  uint8_t ret_code = swi_send_byte(SHA204_SWI_FLAG_CMD);
  if (ret_code != SWI_FUNCTION_RETCODE_SUCCESS)
    return SHA204_COMM_FAIL;

  return swi_send_bytes(count, command);
}

/* Communication functions */

uint8_t atsha204Class::sha204c_wakeup(uint8_t *response)
{
  uint8_t ret_code = sha204p_wakeup();
  if (ret_code != SHA204_SUCCESS)
    return ret_code;

  ret_code = sha204p_receive_response(SHA204_RSP_SIZE_MIN, response);
  if (ret_code != SHA204_SUCCESS)
    return ret_code;

  // Verify status response.
  if (response[SHA204_BUFFER_POS_COUNT] != SHA204_RSP_SIZE_MIN)
    ret_code = SHA204_INVALID_SIZE;
  else if (response[SHA204_BUFFER_POS_STATUS] != SHA204_STATUS_BYTE_WAKEUP)
    ret_code = SHA204_COMM_FAIL;
  else 
  {
    if ((response[SHA204_RSP_SIZE_MIN - SHA204_CRC_SIZE] != 0x33)
      || (response[SHA204_RSP_SIZE_MIN + 1 - SHA204_CRC_SIZE] != 0x43))
      ret_code = SHA204_BAD_CRC;
  }
  if (ret_code != SHA204_SUCCESS)
    delay(SHA204_COMMAND_EXEC_MAX);

  return ret_code;
}

uint8_t atsha204Class::sha204c_resync(uint8_t size, uint8_t *response)
{
  // Try to re-synchronize without sending a Wake token
  // (step 1 of the re-synchronization process).
  uint8_t ret_code = sha204p_resync(size, response);
  if (ret_code == SHA204_SUCCESS)
    return ret_code;

  // We lost communication. Send a Wake pulse and try
  // to receive a response (steps 2 and 3 of the
  // re-synchronization process).
  (void) sha204p_sleep();
  ret_code = sha204c_wakeup(response);

  // Translate a return value of success into one
  // that indicates that the device had to be woken up
  // and might have lost its TempKey.
  return (ret_code == SHA204_SUCCESS ? SHA204_RESYNC_WITH_WAKEUP : ret_code);
}

uint8_t atsha204Class::sha204c_send_and_receive(uint8_t *tx_buffer, uint8_t rx_size, uint8_t *rx_buffer, uint8_t execution_delay, uint8_t execution_timeout)
{
  uint8_t ret_code = SHA204_FUNC_FAIL;
  uint8_t ret_code_resync;
  uint8_t n_retries_send;
  uint8_t n_retries_receive;
  uint8_t i;
  uint8_t status_byte;
  uint8_t count = tx_buffer[SHA204_BUFFER_POS_COUNT];
  uint8_t count_minus_crc = count - SHA204_CRC_SIZE;
  uint16_t execution_timeout_us = (uint16_t) (execution_timeout * 1000) + SHA204_RESPONSE_TIMEOUT;
  volatile uint16_t timeout_countdown;

  // Append CRC.
  sha204c_calculate_crc(count_minus_crc, tx_buffer, tx_buffer + count_minus_crc);

  // Retry loop for sending a command and receiving a response.
  n_retries_send = SHA204_RETRY_COUNT + 1;

  while ((n_retries_send-- > 0) && (ret_code != SHA204_SUCCESS)) 
  {
    // Send command.
    ret_code = sha204p_send_command(count, tx_buffer);
    if (ret_code != SHA204_SUCCESS) 
    {
      if (sha204c_resync(rx_size, rx_buffer) == SHA204_RX_NO_RESPONSE)
        return ret_code; // The device seems to be dead in the water.
      else
        continue;
    }

    // Wait minimum command execution time and then start polling for a response.
    delay(execution_delay);

    // Retry loop for receiving a response.
    n_retries_receive = SHA204_RETRY_COUNT + 1;
    while (n_retries_receive-- > 0) 
    {
      // Reset response buffer.
      for (i = 0; i < rx_size; i++)
        rx_buffer[i] = 0;

      // Poll for response.
      timeout_countdown = execution_timeout_us;
      do 
      {
        ret_code = sha204p_receive_response(rx_size, rx_buffer);
        timeout_countdown -= SHA204_RESPONSE_TIMEOUT;
      } 
      while ((timeout_countdown > SHA204_RESPONSE_TIMEOUT) && (ret_code == SHA204_RX_NO_RESPONSE));

      if (ret_code == SHA204_RX_NO_RESPONSE) 
      {
        // We did not receive a response. Re-synchronize and send command again.
        if (sha204c_resync(rx_size, rx_buffer) == SHA204_RX_NO_RESPONSE)
          // The device seems to be dead in the water.
          return ret_code;
        else
          break;
      }

      // Check whether we received a valid response.
      if (ret_code == SHA204_INVALID_SIZE)
      {
        // We see 0xFF for the count when communication got out of sync.
        ret_code_resync = sha204c_resync(rx_size, rx_buffer);
        if (ret_code_resync == SHA204_SUCCESS)
          // We did not have to wake up the device. Try receiving response again.
          continue;
        if (ret_code_resync == SHA204_RESYNC_WITH_WAKEUP)
          // We could re-synchronize, but only after waking up the device.
          // Re-send command.
          break;
        else
          // We failed to re-synchronize.
          return ret_code;
      }

      // We received a response of valid size.
      // Check the consistency of the response.
      ret_code = sha204c_check_crc(rx_buffer);
      if (ret_code == SHA204_SUCCESS) 
      {
        // Received valid response.
        if (rx_buffer[SHA204_BUFFER_POS_COUNT] > SHA204_RSP_SIZE_MIN)
          // Received non-status response. We are done.
          return ret_code;

        // Received status response.
        status_byte = rx_buffer[SHA204_BUFFER_POS_STATUS];

        // Translate the three possible device status error codes
        // into library return codes.
        if (status_byte == SHA204_STATUS_BYTE_PARSE)
          return SHA204_PARSE_ERROR;
        if (status_byte == SHA204_STATUS_BYTE_EXEC)
          return SHA204_CMD_FAIL;
        if (status_byte == SHA204_STATUS_BYTE_COMM) 
        {
          // In case of the device status byte indicating a communication
          // error this function exits the retry loop for receiving a response
          // and enters the overall retry loop
          // (send command / receive response).
          ret_code = SHA204_STATUS_CRC;
          break;
        }

        // Received status response from CheckMAC, DeriveKey, GenDig,
        // Lock, Nonce, Pause, UpdateExtra, or Write command.
        return ret_code;
      }

      else 
      {
        // Received response with incorrect CRC.
        ret_code_resync = sha204c_resync(rx_size, rx_buffer);
        if (ret_code_resync == SHA204_SUCCESS)
          // We did not have to wake up the device. Try receiving response again.
          continue;
        if (ret_code_resync == SHA204_RESYNC_WITH_WAKEUP)
          // We could re-synchronize, but only after waking up the device.
          // Re-send command.
          break;
        else
          // We failed to re-synchronize.
          return ret_code;
      } // block end of check response consistency

    } // block end of receive retry loop

  } // block end of send and receive retry loop

  return ret_code;
}


/* Marshaling functions */

uint8_t atsha204Class::sha204m_random(uint8_t * tx_buffer, uint8_t * rx_buffer, uint8_t mode)
{
  if (!tx_buffer || !rx_buffer || (mode > RANDOM_NO_SEED_UPDATE))
    return SHA204_BAD_PARAM;

  tx_buffer[SHA204_COUNT_IDX] = RANDOM_COUNT;
  tx_buffer[SHA204_OPCODE_IDX] = SHA204_RANDOM;
  tx_buffer[RANDOM_MODE_IDX] = mode & RANDOM_SEED_UPDATE;

  tx_buffer[RANDOM_PARAM2_IDX] =
    tx_buffer[RANDOM_PARAM2_IDX + 1] = 0;

  return sha204c_send_and_receive(&tx_buffer[0], RANDOM_RSP_SIZE, &rx_buffer[0], RANDOM_DELAY, RANDOM_EXEC_MAX - RANDOM_DELAY);
}

uint8_t atsha204Class::sha204m_dev_rev(uint8_t *tx_buffer, uint8_t *rx_buffer)
{
  if (!tx_buffer || !rx_buffer)
    return SHA204_BAD_PARAM;

  tx_buffer[SHA204_COUNT_IDX] = DEVREV_COUNT;
  tx_buffer[SHA204_OPCODE_IDX] = SHA204_DEVREV;

  // Parameters are 0.
  tx_buffer[DEVREV_PARAM1_IDX] =
    tx_buffer[DEVREV_PARAM2_IDX] =
    tx_buffer[DEVREV_PARAM2_IDX + 1] = 0;

  return sha204c_send_and_receive(&tx_buffer[0], DEVREV_RSP_SIZE, &rx_buffer[0],
  DEVREV_DELAY, DEVREV_EXEC_MAX - DEVREV_DELAY);
}

uint8_t atsha204Class::sha204m_write(uint8_t *tx_buffer, uint8_t *rx_buffer,
			uint8_t zone, uint16_t address, uint8_t *new_value, uint8_t *mac)
{
	uint8_t *p_command;
	uint8_t count;

	if (!tx_buffer || !rx_buffer || !new_value || (zone & ~WRITE_ZONE_MASK))
		// no null pointers allowed
		// zone has to match a valid param1 value.
		return SHA204_BAD_PARAM;

	address >>= 2;
	if ((zone & SHA204_ZONE_MASK) == SHA204_ZONE_CONFIG) {
		if (address > SHA204_ADDRESS_MASK_CONFIG)
			return SHA204_BAD_PARAM;
	}
	else if ((zone & SHA204_ZONE_MASK) == SHA204_ZONE_OTP) {
		if (address > SHA204_ADDRESS_MASK_OTP)
			return SHA204_BAD_PARAM;
	}
	else if ((zone & SHA204_ZONE_MASK) == SHA204_ZONE_DATA) {
		if (address > SHA204_ADDRESS_MASK)
			return SHA204_BAD_PARAM;
	}

	p_command = &tx_buffer[SHA204_OPCODE_IDX];
	*p_command++ = SHA204_WRITE;
	*p_command++ = zone;
	*p_command++ = (uint8_t) (address & SHA204_ADDRESS_MASK);
	*p_command++ = 0;

	count = (zone & SHA204_ZONE_COUNT_FLAG) ? SHA204_ZONE_ACCESS_32 : SHA204_ZONE_ACCESS_4;
	//count = SHA204_ZONE_ACCESS_4;
	memcpy(p_command, new_value, count);
	p_command += count;

	if (mac != NULL)
	{
		memcpy(p_command, mac, WRITE_MAC_SIZE);
		p_command += WRITE_MAC_SIZE;
	}

	// Supply count.
	tx_buffer[SHA204_COUNT_IDX] = (uint8_t) (p_command - &tx_buffer[0] + SHA204_CRC_SIZE);

	uint8_t write_rsp_size = WRITE_RSP_SIZE;
	return sha204c_send_and_receive(&tx_buffer[0], write_rsp_size, &rx_buffer[0], WRITE_DELAY, WRITE_EXEC_MAX - WRITE_DELAY);
}

uint8_t atsha204Class::sha204m_read(uint8_t *tx_buffer, uint8_t *rx_buffer, uint8_t zone, uint16_t address)
{
  uint8_t rx_size;

  if (!tx_buffer || !rx_buffer || ((zone & ~READ_ZONE_MASK) != 0)
    || ((zone & READ_ZONE_MODE_32_BYTES) && (zone == SHA204_ZONE_OTP)))
    return 1;

  address >>= 2;
  if ((zone & SHA204_ZONE_MASK) == SHA204_ZONE_CONFIG) 
  {
    if (address > SHA204_ADDRESS_MASK_CONFIG)
      return SHA204_BAD_PARAM;
  }
  else if ((zone & SHA204_ZONE_MASK) == SHA204_ZONE_OTP) 
  {
    if (address > SHA204_ADDRESS_MASK_OTP)
      return SHA204_BAD_PARAM;
  }
  else if ((zone & SHA204_ZONE_MASK) == SHA204_ZONE_DATA) 
  {
    if (address > SHA204_ADDRESS_MASK)
      return 1;
  }

  tx_buffer[SHA204_COUNT_IDX] = READ_COUNT;
  tx_buffer[SHA204_OPCODE_IDX] = SHA204_READ;
  tx_buffer[READ_ZONE_IDX] = zone;
  tx_buffer[READ_ADDR_IDX] = (uint8_t) (address & SHA204_ADDRESS_MASK);
  tx_buffer[READ_ADDR_IDX + 1] = 0;

  rx_size = (zone & SHA204_ZONE_COUNT_FLAG) ? READ_32_RSP_SIZE : READ_4_RSP_SIZE;

  return sha204c_send_and_receive(&tx_buffer[0], rx_size, &rx_buffer[0], READ_DELAY, READ_EXEC_MAX - READ_DELAY);
}

uint8_t atsha204Class::sha204m_execute(uint8_t op_code, uint8_t param1, uint16_t param2,
			uint8_t datalen1, uint8_t *data1, uint8_t datalen2, uint8_t *data2, uint8_t datalen3, uint8_t *data3,
			uint8_t tx_size, uint8_t *tx_buffer, uint8_t rx_size, uint8_t *rx_buffer)
{
	uint8_t poll_delay, poll_timeout, response_size;
	uint8_t *p_buffer;
	uint8_t len;

	uint8_t ret_code = sha204m_check_parameters(op_code, param1, param2,
				datalen1, data1, datalen2, data2, datalen3, data3,
				tx_size, tx_buffer, rx_size, rx_buffer);
	if (ret_code != SHA204_SUCCESS)
		return ret_code;

	// Supply delays and response size.
	switch (op_code) 
	{
		case SHA204_CHECKMAC:
			poll_delay = CHECKMAC_DELAY;
			poll_timeout = CHECKMAC_EXEC_MAX - CHECKMAC_DELAY;
			response_size = CHECKMAC_RSP_SIZE;
			break;

		case SHA204_DERIVE_KEY:
			poll_delay = DERIVE_KEY_DELAY;
			poll_timeout = DERIVE_KEY_EXEC_MAX - DERIVE_KEY_DELAY;
			response_size = DERIVE_KEY_RSP_SIZE;
			break;

		case SHA204_DEVREV:
			poll_delay = DEVREV_DELAY;
			poll_timeout = DEVREV_EXEC_MAX - DEVREV_DELAY;
			response_size = DEVREV_RSP_SIZE;
			break;

		case SHA204_GENDIG:
			poll_delay = GENDIG_DELAY;
			poll_timeout = GENDIG_EXEC_MAX - GENDIG_DELAY;
			response_size = GENDIG_RSP_SIZE;
			break;

		case SHA204_HMAC:
			poll_delay = HMAC_DELAY;
			poll_timeout = HMAC_EXEC_MAX - HMAC_DELAY;
			response_size = HMAC_RSP_SIZE;
			break;

		case SHA204_LOCK:
			poll_delay = LOCK_DELAY;
			poll_timeout = LOCK_EXEC_MAX - LOCK_DELAY;
			response_size = LOCK_RSP_SIZE;
			break;

		case SHA204_MAC:
			poll_delay = MAC_DELAY;
			poll_timeout = MAC_EXEC_MAX - MAC_DELAY;
			response_size = MAC_RSP_SIZE;
			break;

		case SHA204_NONCE:
			poll_delay = NONCE_DELAY;
			poll_timeout = NONCE_EXEC_MAX - NONCE_DELAY;
			response_size = param1 == NONCE_MODE_PASSTHROUGH
								? NONCE_RSP_SIZE_SHORT : NONCE_RSP_SIZE_LONG;
			break;

		case SHA204_PAUSE:
			poll_delay = PAUSE_DELAY;
			poll_timeout = PAUSE_EXEC_MAX - PAUSE_DELAY;
			response_size = PAUSE_RSP_SIZE;
			break;

		case SHA204_RANDOM:
			poll_delay = RANDOM_DELAY;
			poll_timeout = RANDOM_EXEC_MAX - RANDOM_DELAY;
			response_size = RANDOM_RSP_SIZE;
			break;

		case SHA204_READ:
			poll_delay = READ_DELAY;
			poll_timeout = READ_EXEC_MAX - READ_DELAY;
			response_size = (param1 & SHA204_ZONE_COUNT_FLAG)
								? READ_32_RSP_SIZE : READ_4_RSP_SIZE;
			break;

		case SHA204_UPDATE_EXTRA:
			poll_delay = UPDATE_DELAY;
			poll_timeout = UPDATE_EXEC_MAX - UPDATE_DELAY;
			response_size = UPDATE_RSP_SIZE;
			break;

		case SHA204_WRITE:
			poll_delay = WRITE_DELAY;
			poll_timeout = WRITE_EXEC_MAX - WRITE_DELAY;
			response_size = WRITE_RSP_SIZE;
			break;

		default:
			poll_delay = 0;
			poll_timeout = SHA204_COMMAND_EXEC_MAX;
			response_size = rx_size;
	}

	// Assemble command.
	len = datalen1 + datalen2 + datalen3 + SHA204_CMD_SIZE_MIN;
	p_buffer = tx_buffer;
	*p_buffer++ = len;
	*p_buffer++ = op_code;
	*p_buffer++ = param1;
	*p_buffer++ = param2 & 0xFF;
	*p_buffer++ = param2 >> 8;

	if (datalen1 > 0) {
		memcpy(p_buffer, data1, datalen1);
		p_buffer += datalen1;
	}
	if (datalen2 > 0) {
		memcpy(p_buffer, data2, datalen2);
		p_buffer += datalen2;
	}
	if (datalen3 > 0) {
		memcpy(p_buffer, data3, datalen3);
		p_buffer += datalen3;
	}

	sha204c_calculate_crc(len - SHA204_CRC_SIZE, tx_buffer, p_buffer);

	// Send command and receive response.
	return sha204c_send_and_receive(&tx_buffer[0], response_size,
				&rx_buffer[0],	poll_delay, poll_timeout);
}

uint8_t atsha204Class::sha204m_check_parameters(uint8_t op_code, uint8_t param1, uint16_t param2,
			uint8_t datalen1, uint8_t *data1, uint8_t datalen2, uint8_t *data2, uint8_t datalen3, uint8_t *data3,
			uint8_t tx_size, uint8_t *tx_buffer, uint8_t rx_size, uint8_t *rx_buffer)
{
#ifdef SHA204_CHECK_PARAMETERS

	uint8_t len = datalen1 + datalen2 + datalen3 + SHA204_CMD_SIZE_MIN;
	if (!tx_buffer || tx_size < len || rx_size < SHA204_RSP_SIZE_MIN || !rx_buffer)
		return SHA204_BAD_PARAM;

	if ((datalen1 > 0 && !data1) || (datalen2 > 0 && !data2) || (datalen3 > 0 && !data3))
		return SHA204_BAD_PARAM;

	// Check parameters depending on op-code.
	switch (op_code) 
	{
		case SHA204_CHECKMAC:
			if (
					// no null pointers allowed
					!data1 || !data2
					// No reserved bits should be set.
					|| (param1 | CHECKMAC_MODE_MASK) != CHECKMAC_MODE_MASK
					// key_id > 15 not allowed
					|| param2 > SHA204_KEY_ID_MAX
				)
				return SHA204_BAD_PARAM;
			break;

		case SHA204_DERIVE_KEY:
			if (param2 > SHA204_KEY_ID_MAX)
				return SHA204_BAD_PARAM;
			break;

		case SHA204_DEVREV:
			break;

		case SHA204_GENDIG:
			if ((param1 != GENDIG_ZONE_OTP) && (param1 != GENDIG_ZONE_DATA))
				return SHA204_BAD_PARAM;
			break;

		case SHA204_HMAC:
			if ((param1 & ~HMAC_MODE_MASK) != 0)
				return SHA204_BAD_PARAM;
			break;

		case SHA204_LOCK:
			if (((param1 & ~LOCK_ZONE_MASK) != 0)
						|| ((param1 & LOCK_ZONE_NO_CRC) && (param2 != 0)))
				return SHA204_BAD_PARAM;
			break;

		case SHA204_MAC:
			if (((param1 & ~MAC_MODE_MASK) != 0)
						|| (((param1 & MAC_MODE_BLOCK2_TEMPKEY) == 0) && !data1))
				return SHA204_BAD_PARAM;
			break;

		case SHA204_NONCE:
			if (  !data1
					|| (param1 > NONCE_MODE_PASSTHROUGH)
					|| (param1 == NONCE_MODE_INVALID)
				)
				return SHA204_BAD_PARAM;
			break;

		case SHA204_PAUSE:
			break;

		case SHA204_RANDOM:
			if (param1 > RANDOM_NO_SEED_UPDATE)
				return SHA204_BAD_PARAM;
			break;

		case SHA204_READ:
			if (((param1 & ~READ_ZONE_MASK) != 0)
						|| ((param1 & READ_ZONE_MODE_32_BYTES) && (param1 == SHA204_ZONE_OTP)))
				return SHA204_BAD_PARAM;
			break;

		case SHA204_TEMPSENSE:
			break;

		case SHA204_UPDATE_EXTRA:
			if (param1 > UPDATE_CONFIG_BYTE_86)
				return SHA204_BAD_PARAM;
			break;

		case SHA204_WRITE:
			if (!data1 || ((param1 & ~WRITE_ZONE_MASK) != 0))
				return SHA204_BAD_PARAM;
			break;

		default:
			// unknown op-code
			return SHA204_BAD_PARAM;
	}

	return SHA204_SUCCESS;

#else
	return SHA204_SUCCESS;
#endif
}



/* CRC Calculator and Checker */
void atsha204Class::sha204c_calculate_crc(uint8_t length, uint8_t *data, uint8_t *crc) 
{
  uint8_t counter;
  uint16_t crc_register = 0;
  uint16_t polynom = 0x8005;
  uint8_t shift_register;
  uint8_t data_bit, crc_bit;

  for (counter = 0; counter < length; counter++)
  {
    for (shift_register = 0x01; shift_register > 0x00; shift_register <<= 1) 
    {
      data_bit = (data[counter] & shift_register) ? 1 : 0;
      crc_bit = crc_register >> 15;

      // Shift CRC to the left by 1.
      crc_register <<= 1;

      if ((data_bit ^ crc_bit) != 0)
        crc_register ^= polynom;
    }
  }
  crc[0] = (uint8_t) (crc_register & 0x00FF);
  crc[1] = (uint8_t) (crc_register >> 8);
}

uint8_t atsha204Class::sha204c_check_crc(uint8_t *response)
{
  uint8_t crc[SHA204_CRC_SIZE];
  uint8_t count = response[SHA204_BUFFER_POS_COUNT];

  count -= SHA204_CRC_SIZE;
  sha204c_calculate_crc(count, response, crc);

  return (crc[0] == response[count] && crc[1] == response[count + 1])
    ? SHA204_SUCCESS : SHA204_BAD_CRC;
}

uint8_t atsha204Class::sha204e_configure_key()
{
	// declared as "volatile" for easier debugging
	volatile uint8_t ret_code;

	const uint8_t config_child = 0x7D;
	const uint8_t config_parent = 0xCD;
	const uint8_t config_address = 32;
	
	// Make the command buffer the long size (32 bytes, no MAC) of the Write command.
	uint8_t command[WRITE_COUNT_LONG];
	
	uint8_t data_load[SHA204_ZONE_ACCESS_32];

	// Make the response buffer the size of a Read response.
	uint8_t response[READ_32_RSP_SIZE];

	// Wake up the client device.
	//ret_code = sha204e_wakeup_device(SHA204_CLIENT_ADDRESS);
	//if (ret_code != SHA204_SUCCESS)
	//	return ret_code;
	
	// Read client device configuration for child key.
	memset(response, 0, sizeof(response));
	ret_code = sha204m_read(command, response, SHA204_ZONE_COUNT_FLAG | SHA204_ZONE_CONFIG, config_address);
	if (ret_code != SHA204_SUCCESS) {
	//	sha204p_sleep();
		return ret_code;
	}

	// Check whether we configured already. If so, exit here.
	if ((response[SHA204_BUFFER_POS_DATA + 9] == config_child)
		&& (response[SHA204_BUFFER_POS_DATA + 15] == config_parent)) {
	//	sha204p_sleep();
		return ret_code;
	}

	// Write client configuration.
	memcpy(data_load, &response[SHA204_BUFFER_POS_DATA], sizeof(data_load));
	data_load[9] = config_child;
	data_load[14] = config_parent;
	ret_code = sha204m_write(command, response, SHA204_ZONE_COUNT_FLAG | SHA204_ZONE_CONFIG,
							config_address, data_load, NULL);
	if (ret_code != SHA204_SUCCESS) {
	//	sha204p_sleep();
		return ret_code;
	}

	//sha204p_sleep();
	
	return ret_code;
}

uint8_t atsha204Class::sha204e_read_config_zone(uint8_t *config_data)
{
	// declared as "volatile" for easier debugging
	volatile uint8_t ret_code;
	
	uint16_t config_address;
	
	// Make the command buffer the size of the Read command.
	uint8_t command[READ_COUNT];

	// Make the response buffer the size of the maximum Read response.
	uint8_t response[READ_32_RSP_SIZE];
	
	// Use this buffer to read the last 24 bytes in 4-byte junks.
	uint8_t response_read_4[READ_4_RSP_SIZE];
	
	uint8_t *p_response;

	//sha204p_init();

	//sha204p_set_device_id(device_id);

	// Read first 32 bytes. Put a breakpoint after the read and inspect "response" to obtain the data.
	//ret_code = sha204c_wakeup(response);
	//if (ret_code != SHA204_SUCCESS)
	//	return ret_code;
		
	memset(response, 0, sizeof(response));
	config_address = 0;
	ret_code = sha204m_read(command, response, SHA204_ZONE_CONFIG | READ_ZONE_MODE_32_BYTES, config_address);
	//sha204p_sleep();
	//if (ret_code != SHA204_SUCCESS)
	//	return ret_code;
		
	if (config_data) {
		memcpy(config_data, &response[SHA204_BUFFER_POS_DATA], SHA204_ZONE_ACCESS_32);
		config_data += SHA204_ZONE_ACCESS_32;
	}		
	// Read second 32 bytes. Put a breakpoint after the read and inspect "response" to obtain the data.
	memset(response, 0, sizeof(response));
	//ret_code = sha204c_wakeup(response);
	//if (ret_code != SHA204_SUCCESS)
	//	return ret_code;

	config_address += SHA204_ZONE_ACCESS_32;
	memset(response, 0, sizeof(response));
	ret_code = sha204m_read(command, response, SHA204_ZONE_CONFIG | READ_ZONE_MODE_32_BYTES, config_address);
	//sha204p_sleep();
	//if (ret_code != SHA204_SUCCESS)
	//	return ret_code;
		
	if (config_data) {
		memcpy(config_data, &response[SHA204_BUFFER_POS_DATA], SHA204_ZONE_ACCESS_32);
		config_data += SHA204_ZONE_ACCESS_32;
	}
		
	// Read last 24 bytes in six four-byte junks.
	memset(response, 0, sizeof(response));
	//ret_code = sha204c_wakeup(response);
	//if (ret_code != SHA204_SUCCESS)
	//	return ret_code;
	
	config_address += SHA204_ZONE_ACCESS_32;
	response[SHA204_BUFFER_POS_COUNT] = 0;
	p_response = &response[SHA204_BUFFER_POS_DATA];
	memset(response, 0, sizeof(response));
	while (config_address < SHA204_CONFIG_SIZE) {
		memset(response_read_4, 0, sizeof(response_read_4));
		ret_code = sha204m_read(command, response_read_4, SHA204_ZONE_CONFIG, config_address);
		if (ret_code != SHA204_SUCCESS) {
			sha204p_sleep();
			return ret_code;
		}
		memcpy(p_response, &response_read_4[SHA204_BUFFER_POS_DATA], SHA204_ZONE_ACCESS_4);
		p_response += SHA204_ZONE_ACCESS_4;
		response[SHA204_BUFFER_POS_COUNT] += SHA204_ZONE_ACCESS_4; // Update count byte in virtual response packet.
		config_address += SHA204_ZONE_ACCESS_4;
	}	
	// Put a breakpoint here and inspect "response" to obtain the data.
	//sha204p_sleep();
		
	if (ret_code == SHA204_SUCCESS && config_data)
		memcpy(config_data, &response[SHA204_BUFFER_POS_DATA], SHA204_CONFIG_SIZE - 2 * SHA204_ZONE_ACCESS_32);

	return ret_code;
}

uint8_t atsha204Class::sha204e_lock_config_zone()
{
	uint8_t ret_code;
	uint8_t config_data[SHA204_CONFIG_SIZE];
	uint8_t crc_array[SHA204_CRC_SIZE];
	uint16_t crc;
	uint8_t command[LOCK_COUNT];
	uint8_t response[LOCK_RSP_SIZE];
	
	//sha204p_sleep();
	
	ret_code = sha204e_read_config_zone(config_data);
	if (ret_code != SHA204_SUCCESS)
		return ret_code;
		
	// Check whether the configuration zone is locked already.
	if (config_data[87] == 0)
		return ret_code;
	
	sha204c_calculate_crc(sizeof(config_data), config_data, crc_array);
	crc = (crc_array[1] << 8) + crc_array[0];

	//ret_code = sha204c_wakeup(response);
	ret_code = sha204m_lock(command, response, SHA204_ZONE_CONFIG, crc);
	
	return ret_code;
}


/** \brief This function sends a Lock command to the device.
 *
 * \param[in]  tx_buffer pointer to transmit buffer
 * \param[out] rx_buffer pointer to receive buffer
 * \param[in]  zone zone id to lock
 * \param[in]  summary zone digest
 * \return status of the operation
 */
uint8_t atsha204Class::sha204m_lock(uint8_t *tx_buffer, uint8_t *rx_buffer, uint8_t zone, uint16_t summary)
{
	if (!tx_buffer || !rx_buffer || (zone & ~LOCK_ZONE_MASK)
				|| ((zone & LOCK_ZONE_NO_CRC) && summary))
		// no null pointers allowed
		// zone has to match an allowed zone.
		// If no CRC is required summary has to be 0.
		return SHA204_BAD_PARAM;

	tx_buffer[SHA204_COUNT_IDX] = LOCK_COUNT;
	tx_buffer[SHA204_OPCODE_IDX] = SHA204_LOCK;
	tx_buffer[LOCK_ZONE_IDX] = zone & LOCK_ZONE_MASK;
	tx_buffer[LOCK_SUMMARY_IDX]= summary & 0xFF;
	tx_buffer[LOCK_SUMMARY_IDX + 1]= summary >> 8;
	return sha204c_send_and_receive(&tx_buffer[0], LOCK_RSP_SIZE, &rx_buffer[0],
				LOCK_DELAY, LOCK_EXEC_MAX - LOCK_DELAY);
}

uint8_t atsha204Class::sha204e_configure_derive_key()
{
	// declared as "volatile" for easier debugging
	volatile uint8_t ret_code;

	// Configure key.
	ret_code = sha204e_configure_key();
	if (ret_code != SHA204_SUCCESS)
		return ret_code;
	
//#if (SHA204_EXAMPLE_CONFIG_WITH_LOCK != 0)
	ret_code = sha204e_lock_config_zone();
//#endif

	return ret_code;
}
/**
*	Derive Key command
*/
uint8_t atsha204Class::sha204m_derive_key(uint8_t *tx_buffer, uint8_t *rx_buffer,
			uint8_t random, uint8_t target_key, uint8_t *mac)
{
	if (!tx_buffer || !rx_buffer || (random & ~DERIVE_KEY_RANDOM_FLAG)
				 || (target_key > SHA204_KEY_ID_MAX))
		// no null pointers allowed
		// random has to match an allowed DeriveKey mode.
		// target_key > 15 not allowed
		return SHA204_BAD_PARAM;

	tx_buffer[SHA204_OPCODE_IDX] = SHA204_DERIVE_KEY;
	tx_buffer[DERIVE_KEY_RANDOM_IDX] = random;
	tx_buffer[DERIVE_KEY_TARGETKEY_IDX] = target_key;
	tx_buffer[DERIVE_KEY_TARGETKEY_IDX + 1] = 0;
	if (mac != NULL)
	{
		memcpy(&tx_buffer[DERIVE_KEY_MAC_IDX], mac, DERIVE_KEY_MAC_SIZE);
		tx_buffer[SHA204_COUNT_IDX] = DERIVE_KEY_COUNT_LARGE;
	}
	else
		tx_buffer[SHA204_COUNT_IDX] = DERIVE_KEY_COUNT_SMALL;

		
	return sha204c_send_and_receive(&tx_buffer[0], DERIVE_KEY_RSP_SIZE, &rx_buffer[0],
				DERIVE_KEY_DELAY, DERIVE_KEY_EXEC_MAX - DERIVE_KEY_DELAY);
}


uint8_t atsha204Class::sha204m_nonce(uint8_t *tx_buffer, uint8_t *rx_buffer, uint8_t mode, uint8_t *numin)
{
	uint8_t rx_size;

	if (!tx_buffer || !rx_buffer || !numin
				|| (mode > NONCE_MODE_PASSTHROUGH) || (mode == NONCE_MODE_INVALID))
		// no null pointers allowed
		// mode has to match an allowed Nonce mode.
		return SHA204_BAD_PARAM;

	tx_buffer[SHA204_OPCODE_IDX] = SHA204_NONCE;
	tx_buffer[NONCE_MODE_IDX] = mode;

	// 2. parameter is 0.
	tx_buffer[NONCE_PARAM2_IDX] =
	tx_buffer[NONCE_PARAM2_IDX + 1] = 0;

	if (mode != NONCE_MODE_PASSTHROUGH)
	{
		memcpy(&tx_buffer[NONCE_INPUT_IDX], numin, NONCE_NUMIN_SIZE);
		tx_buffer[SHA204_COUNT_IDX] = NONCE_COUNT_SHORT;
		rx_size = NONCE_RSP_SIZE_LONG;
	}
	else
	{
		memcpy(&tx_buffer[NONCE_INPUT_IDX], numin, NONCE_NUMIN_SIZE_PASSTHROUGH);
		tx_buffer[SHA204_COUNT_IDX] = NONCE_COUNT_LONG;
		rx_size = NONCE_RSP_SIZE_SHORT;
	}

	return sha204c_send_and_receive(&tx_buffer[0], rx_size, &rx_buffer[0],
				NONCE_DELAY, NONCE_EXEC_MAX - NONCE_DELAY);
}

uint8_t atsha204Class::sha204e_configure_diversify_key(void)
{
	// declared as "volatile" for easier debugging
	volatile uint8_t ret_code;
	
	uint8_t command[NONCE_COUNT_LONG];
	uint8_t response[SHA204_RSP_SIZE_MIN];
	uint8_t data_load[NONCE_NUMIN_SIZE_PASSTHROUGH];

	// Configure key. -> I did this manually
	ret_code = sha204e_configure_key();
	if (ret_code != SHA204_SUCCESS)
		return ret_code;

	
	// Read serial number and pad it.
	memset(data_load, 0, sizeof(data_load));
	ret_code = getSerialNumber(data_load);
	if (ret_code != SHA204_SUCCESS) {
		//sha204p_sleep();
		return ret_code;
	}
	
	//  Put padded serial number into TempKey (fixed Nonce).
	ret_code = sha204m_nonce(command, response, NONCE_MODE_PASSTHROUGH, data_load);
	if (ret_code != SHA204_SUCCESS) {
		//sha204p_sleep();
		return ret_code;
	}
	
	//  Send DeriveKey command.
	ret_code = sha204m_derive_key(command, response, DERIVE_KEY_RANDOM_FLAG, 1, NULL);
	/*
#ifdef SHA204_EXAMPLE_CONFIG_WITH_LOCK
	sha204p_sleep();

	if (ret_code != SHA204_SUCCESS)
		return ret_code;

	ret_code = sha204e_lock_config_zone(SHA204_HOST_ADDRESS);
#endif
*/
	// Put client device to sleep.
	//sha204p_sleep();
	
	return ret_code;
}


uint8_t atsha204Class::sha204m_gen_dig(uint8_t *tx_buffer, uint8_t *rx_buffer,
			uint8_t zone, uint8_t key_id, uint8_t *other_data)
{
	if (!tx_buffer || !rx_buffer || (zone > GENDIG_ZONE_DATA))
		// no null pointers allowed
		// zone has to match a zone (Config, Data, or OTP zone)
		return SHA204_BAD_PARAM;

	if (((zone == GENDIG_ZONE_OTP) && (key_id > SHA204_OTP_BLOCK_MAX))
				|| ((zone == GENDIG_ZONE_DATA) && (key_id > SHA204_KEY_ID_MAX)))
		// If OTP zone is used only valid OTP block values can be used.
		// If Data zone is used key_id > 15 is not allowed.
		return SHA204_BAD_PARAM;

	tx_buffer[SHA204_OPCODE_IDX] = SHA204_GENDIG;
	tx_buffer[GENDIG_ZONE_IDX] = zone;
	tx_buffer[GENDIG_KEYID_IDX] = key_id;
	tx_buffer[GENDIG_KEYID_IDX + 1] = 0;
	if (other_data != NULL)
	{
		memcpy(&tx_buffer[GENDIG_DATA_IDX], other_data, GENDIG_OTHER_DATA_SIZE);
		tx_buffer[SHA204_COUNT_IDX] = GENDIG_COUNT_DATA;
	}
	else
		tx_buffer[SHA204_COUNT_IDX] = GENDIG_COUNT;

	return sha204c_send_and_receive(&tx_buffer[0], GENDIG_RSP_SIZE, &rx_buffer[0],
				GENDIG_DELAY, GENDIG_EXEC_MAX - GENDIG_DELAY);

}

uint8_t atsha204Class::sha204m_mac(uint8_t *tx_buffer, uint8_t *rx_buffer,
			uint8_t mode, uint16_t key_id, uint8_t *challenge)
{
	if (!tx_buffer || !rx_buffer || (mode & ~MAC_MODE_MASK)
				|| (!(mode & MAC_MODE_BLOCK2_TEMPKEY) && !challenge))
		// no null pointers allowed
		// mode has to match an allowed MAC mode.
		// If mode requires challenge data challenge cannot be null.
		return SHA204_BAD_PARAM;

	tx_buffer[SHA204_COUNT_IDX] = MAC_COUNT_SHORT;
	tx_buffer[SHA204_OPCODE_IDX] = SHA204_MAC;
	tx_buffer[MAC_MODE_IDX] = mode;
	tx_buffer[MAC_KEYID_IDX] = key_id & 0xFF;
	tx_buffer[MAC_KEYID_IDX + 1] = key_id >> 8;
	if ((mode & MAC_MODE_BLOCK2_TEMPKEY) == 0)
	{
		memcpy(&tx_buffer[MAC_CHALLENGE_IDX], challenge, MAC_CHALLENGE_SIZE);
		tx_buffer[SHA204_COUNT_IDX] = MAC_COUNT_LONG;
	}

	return sha204c_send_and_receive(&tx_buffer[0], MAC_RSP_SIZE, &rx_buffer[0],
				MAC_DELAY, MAC_EXEC_MAX - MAC_DELAY);
}
