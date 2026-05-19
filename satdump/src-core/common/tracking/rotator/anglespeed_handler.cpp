#include "anglespeed_handler.h"
#include "imgui/imgui.h"
#include "core/style.h"
#include "logger.h"
#include <iomanip>
#include <sstream>

namespace rotator {

  static const std::string DLE = "5555";
  static const std::string ETX = "\r\n";
  static const size_t PAYLOAD_SIZE = 22;

  AngleSpeedHandler::AngleSpeedHandler() : serial(io_service), running(false), read_buffer(max_buffer_size) {}

  AngleSpeedHandler::~AngleSpeedHandler() {
    l_disconnect();
  }

  std::string crc(const std::string &buf) {
    try {
      unsigned char sum = 0;
      for (char c: buf) {
        sum += static_cast<unsigned char>(c);
      }
      std::stringstream ss;
      ss << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << (256 - sum);
      return ss.str();
    } catch (const std::exception &e) {
      logger->error("CRC calculation error: %s", e.what());
      return "00";
    }
  }

  std::string format_angle(float angle) {
    try {
      float v = std::max(-999.99f, std::min(angle, 999.99f));
      char numSign = (v >= 0) ? '+' : '-';
      int valX100 = static_cast<int>(std::abs(v) * 100);
      std::stringstream ss;
      ss << numSign << std::setfill('0') << std::setw(5) << valX100;
      return ss.str();
    } catch (const std::exception &e) {
      logger->error("Angle formatting error: %s", e.what());
      return "+00000";
    }
  }

  std::string format_velocity(float velocity) {
    try {
      float v = std::max(-99.999f, std::min(velocity, 99.999f));
      char numSign = (v >= 0) ? '+' : '-';
      int valX1000 = static_cast<int>(std::abs(v) * 1000);
      std::stringstream ss;
      ss << numSign << std::setfill('0') << std::setw(5) << valX1000;
      return ss.str();
    } catch (const std::exception &e) {
      logger->error("Velocity formatting error: %s", e.what());
      return "+00000";
    }
  }

  void AngleSpeedHandler::process_serial_data(const boost::system::error_code &error, size_t bytes_transferred) {
    if (!running) return;

    if (error) {
      logger->error("Serial error: %s", error.message().c_str());
      l_disconnect();
      return;
    }

    try {
      std::string data(boost::asio::buffer_cast<const char*>(read_buffer.data()), bytes_transferred);
      read_buffer.consume(read_buffer.size()); // Clear buffer

      std::regex pattern("^(\\d{4})(\\d{2})(\\d{4})([+-]\\d{5})([+-]\\d{5})(\\d{4})(.*)");
      std::smatch matches;

      logger->trace("RX: %s", data.c_str());
      if (std::regex_search(data, matches, pattern)) {
        process_packet(matches);
      }else{
        logger->warn("Message format is wrong!");
      }

      if (running) {
        boost::asio::async_read_until(
          serial,
          read_buffer,
          ETX,
          std::bind(&AngleSpeedHandler::process_serial_data, this,
                    std::placeholders::_1,
                    std::placeholders::_2)
        );
      }
    } catch (const std::exception &e) {
      logger->error("Packet processing error: %s", e.what());
      if (running) start_read();
    }
  }

  void AngleSpeedHandler::process_packet(const std::smatch &matches) {
    try {
      std::string state = matches[3];
      uint16_t state_ctrl = std::stoi(state.substr(0, 2), nullptr, 16);

      std::string tail = matches[7];
      if (matches[1] != DLE) {
        logger->warn("Invalid packet beginning");
        return;
      }

      std::string payload = matches.str().substr(DLE.length(), PAYLOAD_SIZE);
      if (tail.substr(0, 2) != crc(payload)) {
        logger->warn("Invalid CRC");
        return;
      }

      std::lock_guard<std::mutex> lock(data_mutex);
      process_azimuth(state_ctrl, matches[4]);
      process_elevation(state_ctrl, matches[5]);

    } catch (const std::exception &e) {
      logger->error("Packet parsing error: %s", e.what());
    }
  }

  void AngleSpeedHandler::process_azimuth(uint16_t state_ctrl, const std::string &az_str) {
    uint16_t az_state = state_ctrl & 0xA000;
    switch (az_state) {
      case 0x8000:
        logger->error("Azimuth sensor fail");
        break;
      case 0x4000:
        logger->error("Azimuth angle fail");
        break;
      default:
        try {
          current_azimuth = std::stof(az_str) * 0.01f;
        } catch (const std::exception &e) {
          logger->error("Azimuth conversion error: %s", e.what());
        }
        break;
    }
  }

  void AngleSpeedHandler::process_elevation(uint16_t state_ctrl, const std::string &el_str) {
    uint16_t el_state = state_ctrl & 0x5000;
    switch (el_state) {
      case 0x2000:
        logger->error("Elevation sensor fail");
        break;
      case 0x1000:
        logger->error("Elevation angle fail");
        break;
      default:
        try {
          current_elevation = std::stof(el_str) * 0.01f;
        } catch (const std::exception &e) {
          logger->error("Elevation conversion error: %s", e.what());
        }
        break;
    }
  }

  void AngleSpeedHandler::start_read() {
    boost::asio::async_read_until(
      serial,
      read_buffer,
      ETX,
      std::bind(&AngleSpeedHandler::process_serial_data, this,
                std::placeholders::_1,
                std::placeholders::_2)
    );
  }

  void AngleSpeedHandler::l_connect(char *address, int baudrate) {
    if (is_connected()) {
      l_disconnect();
    }

    try {
      serial.open(address);
      serial.set_option(boost::asio::serial_port_base::baud_rate(baudrate));
      serial.set_option(boost::asio::serial_port_base::character_size(8));
      serial.set_option(boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one));
      serial.set_option(boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none));
      serial.set_option(boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none));

      running = true;
      work = std::make_unique<boost::asio::io_service::work>(io_service);
      start_read();

      logger->info("Connected to %s", address);
    } catch (const std::exception &e) {
      logger->error("Connection error: %s", e.what());
      l_disconnect();
    }
  }

  void AngleSpeedHandler::l_disconnect() {
    running = false;

    try {
      if (serial.is_open()) serial.close();
      io_service.stop();
      io_service.reset();

      logger->info("Disconnected from %s", input_address);
    } catch (const std::exception &e) {
      logger->error("Disconnection error: %s", e.what());
    }
  }

  rotator_status_t AngleSpeedHandler::get_pos(float *az, float *el) {
    if (!is_connected()) return ROT_ERROR_CON;

    try {
      std::lock_guard<std::mutex> lock(data_mutex);
      *az = current_azimuth;
      *el = current_elevation;
      return ROT_ERROR_OK;
    } catch (const std::exception &e) {
      logger->error("Position read error: %s", e.what());
      return ROT_ERROR_CON;
    }
  }

  rotator_status_t AngleSpeedHandler::set_pos(float az, float el, float az_vel, float el_vel) {
    if (!is_connected()) return ROT_ERROR_CON;

    try {
      if (seq_ >= 100) seq_ = 0;

      std::stringstream payload;
      payload << std::setfill('0') << std::setw(2) << seq_++;
      payload << std::setfill('0') << std::setw(2) << 1;  // DIRECT
      payload << "T00000";
      payload << format_angle(az);
      payload << format_angle(el);
      payload << format_velocity(az_vel);
      payload << format_velocity(el_vel);

      std::string packet = DLE + payload.str() + crc(payload.str()) + ETX;
      auto buf = boost::asio::buffer(packet);
      boost::asio::write(serial, buf);

      logger->trace("TX: %s, size: %d", packet.c_str(), buf.size());
      return ROT_ERROR_OK;
    } catch (const std::exception &e) {
      logger->error("Position set error: %s", e.what());
      l_disconnect();
      return ROT_ERROR_CON;
    }
  }

  std::string AngleSpeedHandler::get_id() {
    return "anglespeed";
  }

  void AngleSpeedHandler::set_settings(nlohmann::json settings) {
    try {
      std::string vaddress = getValueOrDefault(settings["serialport"], std::string(input_address));
      memcpy(input_address, vaddress.data(), vaddress.size());
    } catch (const std::exception &e) {
      logger->error("Settings update error: %s", e.what());
    }
  }

  nlohmann::json AngleSpeedHandler::get_settings() {
    try {
      nlohmann::json v;
      v["serialport"] = std::string(input_address);
      return v;
    } catch (const std::exception &e) {
      logger->error("Settings read error: %s", e.what());
      return nlohmann::json::object();
    }
  }

  void AngleSpeedHandler::render() {
    if (is_connected()) style::beginDisabled();
    ImGui::InputText("Serial Port##serialport", input_address, sizeof(input_address));
    if (is_connected()) style::endDisabled();

    if (is_connected()) {
      io_service.poll();
      if (ImGui::Button("Disconnect##serialdisconnect"))
        disconnect();
    } else {
      if (ImGui::Button("Connect##serialdisconnect"))
        connect();
    }
  }

  bool AngleSpeedHandler::is_connected() {
    return serial.is_open() && running;
  }

  void AngleSpeedHandler::connect() {
    l_connect(input_address, baud_rate);
  }

  void AngleSpeedHandler::disconnect() {
    l_disconnect();
  }

}