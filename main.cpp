#include <iostream>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>

using boost::asio::ip::tcp;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

#define DEBUG

#define IMG_SIZE 1280 * 720 * 3

#define FRAME_HEADER_TEMPLATE "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %llu\r\n\r\n"

const char* jpg_start = "\xff\xd8\xff";
const size_t jpg_start_size = strlen(jpg_start);
const char* jpg_end = "\xff\xd9";
const size_t jpg_end_size = strlen(jpg_end);

void setup_config();
void handle_http_connection(tcp::socket http_client);
void handle_http_connections();
void handle_cam_connection(tcp::socket cam_client);
void handle_cam_connections();

struct Img {
	char* data;
	size_t size;
	std::condition_variable* cond;
};

std::map<std::string, Img*> imgs = {};

bool steel_working = true;

unsigned cam_port;
unsigned http_port;

int main() {
	setlocale(LC_ALL, "russian");

	setup_config();

	std::thread(handle_cam_connections).detach();

	std::thread(handle_http_connections).detach();

	getchar();

	steel_working = false;
}

void setup_config() {
	using namespace nlohmann;

	std::ifstream file("config.json");

	json config = json::parse(file);

	cam_port = config["cam_port"];
	http_port = config["http_port"];
}

ptrdiff_t find(const char* data, const size_t data_size, const char* target, const size_t target_size) {
	for (size_t i = 0; i < data_size - target_size + 1; i++) {
		for (size_t j = 0; j < target_size; j++) {
			if (data[i + j] != target[j]) goto end;
		}
		return i;
		end: continue;
	}
	return -1;
}

void handle_http_connection(tcp::socket http_client) {
#ifdef DEBUG
	std::string addr = http_client.remote_endpoint().address().to_string();
#endif

	beast::error_code ec;

	boost::beast::flat_buffer request_buffer;

	http::request<http::string_body> req;
	http::read(http_client, request_buffer, req, ec);
	if (ec) {
#ifdef DEBUG
		std::cerr << "Failed to read request: " << ec.message() << std::endl;
#endif
		http_client.close();
		return;
	}

	std::cout << req.target() << std::endl;

	if (req.target().size() < 36) {
#ifdef DEBUG
		std::cout << "User id is not valid." << std::endl;
		std::cout << "User " << addr << " disconnected" << std::endl;
#endif
		http_client.close();
		return;
	}

	const char* id = req.target().end() - 36;

	if (!imgs.count(id)) {
#ifdef DEBUG
		std::cout << "Id " << id << " not found." << std::endl;
		std::cout << "User " << addr << " disconnected." << std::endl;
#endif
		http_client.close();
		return;
	}

	Img* img = imgs[id];

	http::response<http::empty_body> res{ http::status::ok, req.version() };
	res.set(http::field::content_type, "multipart/x-mixed-replace; boundary=frame");
	res.keep_alive();
	http::response_serializer<http::empty_body> sr{ res };
	http::write_header(http_client, sr, ec);

	if (ec) {

#ifdef DEBUG
		std::cout << "Failed to write header: " << ec.message() << std::endl;
		std::cout << "User " << addr << " disconnected." << std::endl;
#endif
		http_client.close();
		return;
	}
	char* local_img = new char[IMG_SIZE];

	std::mutex m;
	std::unique_lock<std::mutex> lk(m);

	while (steel_working) {
		img->cond->wait(lk);

		size_t local_img_size = img->size;

		memcpy(local_img, img->data, local_img_size);

		char header[80];
#ifdef _WIN32
		int header_size = sprintf_s(header, 80, FRAME_HEADER_TEMPLATE, local_img_size);
#else
		int header_size = sprintf(header, FRAME_HEADER_TEMPLATE, local_img_size);
#endif

		if (header_size <= 0) {
#ifdef DEBUG
			std::cerr << "Failed to create header wtf?" << std::endl;
#endif
			break;
		}

		http_client.send(boost::asio::buffer(header, header_size), 0, ec);
		if (ec) {
#ifdef DEBUG
			std::cerr << "Failed to send frame header: " << ec.message() << std::endl;
#endif
			break;
		}

		http_client.send(boost::asio::buffer(local_img, local_img_size), 0, ec);
		if (ec) {
#ifdef DEBUG
			std::cerr << "Failed to send frame: " << ec.message() << std::endl;
#endif
			break;
		}
	}
#ifdef DEBUG	
	std::cout << "User " << addr << " disconnected." << std::endl;
#endif
	http_client.close();
	delete[] local_img;
}

void handle_http_connections() {
	asio::io_context http_context;
	tcp::acceptor http_acceptor(http_context, tcp::endpoint(tcp::v4(), http_port));

	while (steel_working) {
		try {
			std::thread(handle_http_connection, http_acceptor.accept()).detach();
		}
		catch (boost::system::system_error e) {
#ifdef DEBUG
			std::cerr << "Failed to accept http client: " << e.what() << std::endl;
#endif
		}
	}
}

void handle_cam_connection(tcp::socket cam_client) {
	beast::error_code ec;
	typedef boost::asio::detail::socket_option::integer<SOL_SOCKET, SO_RCVTIMEO> rcv_timeout_option;
	cam_client.set_option(rcv_timeout_option(5000), ec);

	if (ec) {
#ifdef DEBUG
		std::cout << ec.message() << std::endl;
#endif
		cam_client.close();
		return;
	}

	char id[37];
	id[36] = '\0';
	
	size_t len = cam_client.read_some(asio::buffer(id, 36), ec);

#ifdef DEBUG
	std::string addr = cam_client.remote_endpoint().address().to_string();
#endif

	if (len != 36 || ec) {
#ifdef DEBUG
		std::cout << "Cam " << addr << " disconnected." << std::endl;
#endif
		cam_client.close();
		return;
	}

	if (imgs.count(id)) {
#ifdef DEBUG
		std::cout << "Cam with ID = " << id << " exists." << std::endl;
		std::cout << "Cam " << addr << " disconnected." << std::endl;
#endif
		cam_client.close();
		return;
	}

#ifdef DEBUG
	std::cout << "Cam " << id << " connected from " << addr << std::endl;
#endif

	char* buffer = new char[IMG_SIZE];
	size_t buffer_index = 0;

	std::condition_variable cond;

	Img* img = new Img();
	img->data = new char[IMG_SIZE];
	img->cond = &cond;

	imgs[id] = img;

	while (steel_working) {
		size_t packet_size = cam_client.read_some(asio::buffer(buffer + buffer_index, 1024), ec);
		if (ec) {
			std::cerr << "Failed to read frame: " << ec.message() << std::endl;
			break;
		}
		buffer_index += packet_size;

		ptrdiff_t a = find(buffer, buffer_index, jpg_start, jpg_start_size);
		ptrdiff_t b = find(buffer+buffer_index-packet_size, packet_size, jpg_end, jpg_end_size);
		if (a != -1 && b != -1) {
			b += buffer_index - packet_size;
			img->size = b - a + 2;
			memcpy(img->data, buffer + a, img->size);
			img->cond->notify_all();
			memcpy(buffer, buffer + b + 2, buffer_index - b - 2);
			buffer_index -= b + 2;
		}
	}

#ifdef DEBUG
	std::cout << "Cam " << addr << " with id " << id << " disconnected" << std::endl;
#endif 
	
	imgs.erase(id);

	cam_client.close();
	delete[] buffer;
	delete[] img->data;
	delete img;
}

void handle_cam_connections() {
	asio::io_context cam_context;
	tcp::acceptor cam_acceptor(cam_context, tcp::endpoint(tcp::v4(), cam_port));

	while (steel_working) {
		try {
			std::thread(handle_cam_connection, cam_acceptor.accept()).detach();
		}
		catch (boost::system::system_error e) {
#ifdef DEBUG
			std::cerr << "Failed to accept cam: " << e.what() << std::endl;
#endif
		}
	}
}
