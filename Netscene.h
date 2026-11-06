#pragma once
#include <memory>
#include <string>
#include <SFML/Graphics.hpp>
#include "Registry.h"
#include "Components.h"
#include "System.h"
#include "RenderSystem.h"
#include "Scene.h"
#include "Game.h"            // GameState, UpgradeKind
#include "Hub.h"             // Hud (так подключено в вашем main.cpp)
#include "WorldMirror.h"
#include "NetClient.h"

// Сетевая сцена: заменяет GameScene и WaveClearScene.
// Игровые системы здесь НЕ запускаются: мир считает сервер, а сцена только
//   1. отдаёт ввод сетевому слою,
//   2. раз в кадр применяет к Registry самый свежий снимок (WorldMirror),
//   3. рисует Registry вашим RenderSystem и Hud.
// Написана по интерфейсу Scene, который виден в вашем main.cpp (Scene.h я не видел).

class NetScene : public Scene
{
	Registry& reg;
	GameData& data;
	Hud& hud;
	WorldMirror mirror;
	NetClient net;
	ModuleRegistry render;                 // только RenderSystem
	sf::RenderWindow* window = nullptr;
	sf::Font font;
	bool hasFont = false;
	bool started = false;
	int backIndex;                         // куда вернуться при Failed (например, экран входа); -1 = остаёмся
	int pendingSwitch = -1;
	int titleCounter = 0;

	void drawStatus()
	{
		sf::Vector2f size(window->getSize());

		sf::RectangleShape dim(size);
		dim.setFillColor(sf::Color(0, 0, 0, 150));
		window->draw(dim);

		if (!hasFont) return;

		sf::Text text(font, net.status(), 26);
		text.setFillColor(net.state() == NetClient::State::Failed ? sf::Color(240, 90, 90) : sf::Color::White);
		auto b = text.getLocalBounds();
		text.setOrigin({ b.position.x + b.size.x / 2.f, b.position.y + b.size.y / 2.f });
		text.setPosition({ size.x / 2.f, size.y / 2.f });
		window->draw(text);
	}

	void updateTitle()
	{
		if (!window || ++titleCounter < 30) return;
		titleCounter = 0;

		std::string t = "Game - " + net.status();
		if (net.playing())
		{
			t += " - Wave " + std::to_string(data.wave) + " - Score: " + std::to_string(data.score);
			if (net.rtt_ms() >= 0) t += " - ping " + std::to_string(net.rtt_ms()) + " ms";
		}
		window->setTitle(t);
	}

public:
	NetScene(Registry& r, GameData& d, Hud& h, NetClient::Config cfg, std::string token, int backSceneIndex = -1)
		: reg(r), data(d), hud(h), mirror(r, d), net(std::move(cfg), mirror), render(r), backIndex(backSceneIndex)
	{
		net.set_token(std::move(token));

		const char* paths[] = {
			"C:/Windows/Fonts/arial.ttf",
			"C:/Windows/Fonts/segoeui.ttf",
			"/System/Library/Fonts/Supplemental/Arial.ttf",
			"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
			"font.ttf"
		};
		for (auto p : paths)
			if (font.openFromFile(p)) { hasFont = true; break; }
	}

	Registry& GetRegistry() override { return reg; }

	void SetWindow(sf::RenderWindow* win) override
	{
		window = win;
		render.push_module(std::make_shared<RenderSystem>(reg, *window));
	}

	void Init() override
	{
		pendingSwitch = -1;
		if (!started)
		{
			render.Init();
			render.Start();
			started = true;
		}
		mirror.clear();
		net.start();                       // повторный вызов безопасен
	}

	void Update(float, const InputState& input) override
	{
		pendingSwitch = -1;

		net.send_input(input);             // копится и уходит серверу с частотой тика

		if (net.take_dropped())            // игровая сессия оборвалась: убрать старый мир
			mirror.clear();

		mirror.apply_latest();             // самый свежий снимок (старые отбрасываются)

		if (net.state() == NetClient::State::Failed && backIndex >= 0)
			pendingSwitch = backIndex;

		updateTitle();
	}

	void Render(float dt) override
	{
		render.Render(dt);                 // RenderSystem рисует зеркало мира как обычный мир
		if (!window) return;

		const bool live = net.playing() && mirror.received_any();
		if (!live)
		{
			drawStatus();                  // "Connecting...", "Retry in 4 s", "Invalid token" и т.п.
			return;
		}

		const std::uint8_t ph = mirror.phase();    // 0 игра, 1 выбор бонуса, 2 победа, 3 поражение
		const GameState state = ph == 2 ? GameState::Victory
		                      : ph == 3 ? GameState::GameOver
		                                : GameState::Playing;

		hud.Draw(*window, reg, state, data);

		if (ph == 1)
			hud.DrawUpgradePick(*window, data,
				static_cast<UpgradeKind>(mirror.optionA()),
				static_cast<UpgradeKind>(mirror.optionB()));
	}

	void Stop() override
	{
		net.stop();
		mirror.clear();
	}

	int WantsSwitchTo() const override { return pendingSwitch; }
};