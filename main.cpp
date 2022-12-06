#include <iostream>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <string>
#include <vector>

using boost::asio::ip::tcp;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

ptrdiff_t find(const char* data, const size_t data_size, const char* str, const size_t str_size);

//#define DEBUG

#define IMG_SIZE 1280 * 720 * 3

#define FRAME_HEADER_TEMPLATE "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %llu\r\n\r\n"

const char* jpg_start = "\xff\xd8\xff";
const size_t jpg_start_size = strlen(jpg_start);
const char* jpg_end = "\xff\xd9";
const size_t jpg_end_size = strlen(jpg_end);

void handle_connection(tcp::socket http_client);
void handle_connections();

char* img;
size_t img_size = 0;

size_t img_index = 0;

bool is_writing_to_img = false;

int main() {
	setlocale(LC_ALL, "russian");

	beast::error_code ec;

	asio::io_context cam_context;
	tcp::acceptor cam_acceptor(cam_context, tcp::endpoint(tcp::v4(), 8080));

	tcp::socket cam_client = cam_acceptor.accept(ec);

	if (ec) {
		std::cerr << "Failed to accept cam client: " << ec.message() << std::endl;
		return ec.value();
	}

#ifdef DEBUG
	std::cout << "Camera connected." << std::endl;
#endif


	img = new char[IMG_SIZE];

	char* data = new char[IMG_SIZE];

	size_t data_index = 0;

	std::thread(handle_connections).detach();

	for (;;) {
		size_t buffer_size = cam_client.read_some(asio::buffer(data + data_index, 1024), ec);
		if (ec) {
			std::cerr << "Failed to read frame: " << ec.message() << std::endl;
			break;
		}
		data_index += buffer_size;

		ptrdiff_t a = find(data, data_index, jpg_start, jpg_start_size);
		ptrdiff_t b = find(data, data_index, jpg_end, jpg_end_size);
		if (a != -1 && b != -1) {
			img_size = b - a + 2;
			is_writing_to_img = true;
			memcpy(img, data + a, img_size);
			is_writing_to_img = false;
			if (img_index == SIZE_MAX) {
				img_index = 0;
			}
			else {
				img_index++;
			}
			memcpy(data, data + b + 2, data_index - b - 2);
			data_index -= b + 2;
#ifdef DEBUG
			std::cout << "Recived " << img_size << " bytes." << std::endl;
#endif
		}
	}

	delete[] img;
	delete[] data;
	return ec.value();
}

ptrdiff_t find(const char* data, const size_t data_size, const char* str, const size_t str_size) {
	for (size_t i = 0; i < data_size - str_size; i++) {
		bool found = true;
		for (size_t j = 0; j < str_size; j++) {
			if (data[i + j] != str[j]) {
				found = false;
				break;
			}
		}
		if (found) return i;
	}
	return -1;
}

void handle_connection(tcp::socket http_client) {
	beast::error_code ec;

	boost::beast::flat_buffer request_buffer;

	http::request<http::string_body> req;
	http::read(http_client, request_buffer, req, ec);
	if (ec) {
		std::cerr << "Failed to read request: " << ec.message() << std::endl;
	}

	http::response<http::empty_body> res{ http::status::ok, req.version() };
	res.set(http::field::content_type, "multipart/x-mixed-replace; boundary=frame");
	res.keep_alive();
	http::response_serializer<http::empty_body> sr{ res };
	http::write_header(http_client, sr, ec);

	if (ec) {
		std::cerr << "Failed to write header: " << ec.message() << std::endl;
		return;
	}
	char* local_img = new char[IMG_SIZE];
	size_t last_img_index = 0;
	for (;;) {
		while (is_writing_to_img || last_img_index == img_size) {}
		last_img_index = img_index;

		size_t local_img_size = img_size;

		memcpy(local_img, img, local_img_size);

		char header[80];
#if _WIN32
		int header_size = sprintf_s(header, 80, FRAME_HEADER_TEMPLATE, local_img_size);
#else
		int header_size = sprintf(header, FRAME_HEADER_TEMPLATE, local_img_size);
#endif

		if (header_size <= 0) {
			std::cerr << "Failed to create header wtf?" << std::endl;
			break;
		}

		http_client.send(boost::asio::buffer(header, header_size), 0, ec);
		if (ec) {
			std::cerr << "Failed to send frame header: " << ec.message() << std::endl;
			break;
		}
		http_client.send(boost::asio::buffer(local_img, local_img_size), 0, ec);
		if (ec) {
			std::cerr << "Failed to send frame: " << ec.message() << std::endl;
			break;
		}
	}
	delete[] local_img;
}

void handle_connections() {
	asio::io_context http_context;
	tcp::acceptor http_acceptor(http_context, tcp::endpoint(tcp::v4(), 8000));

	for (;;) {
		try {
			std::thread(handle_connection, http_acceptor.accept()).detach();
#ifdef DEBUG
			std::cout << "User connected." << std::endl;
#endif
		}
		catch (boost::system::system_error e) {
			std::cerr << "Failed to accept http client: " << e.what() << std::endl;
		}
	}
}