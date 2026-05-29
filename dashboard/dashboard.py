#!/usr/bin/env python3
import zmq
import json
import time
import math
import statistics
from collections import deque
from rich.live import Live
from rich.table import Table
from rich.layout import Layout
from rich.panel import Panel
from rich.text import Text
from rich import box

class HFTDashboard:
    def __init__(self, endpoint="tcp://127.0.0.1:5555"):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.SUB)
        self.socket.connect(endpoint)
        self.socket.setsockopt_string(zmq.SUBSCRIBE, "")
        
        # State
        self.latest_tick = {}
        self.latest_bar = {}
        self.latest_stats = {}
        self.recent_signals = deque(maxlen=10)
        
        # Risk & Portfolio Metrics
        self.total_trades = 0
        self.winning_trades = 0
        self.pnl_history = deque(maxlen=100) # Keep track of trade PnLs
        self.current_equity = 100000.0       # Starting simulated capital
        self.peak_equity = 100000.0

    def calculate_sharpe(self):
        if len(self.pnl_history) < 3: return 0.0
        # Calculate Returns
        returns = [self.pnl_history[i] - self.pnl_history[i-1] for i in range(1, len(self.pnl_history))]
        mean_ret = statistics.mean(returns)
        std_ret = statistics.stdev(returns)
        if std_ret == 0: return 0.0
        # Annualization factor for high frequency is tricky, we use a basic info ratio
        return (mean_ret / std_ret) * math.sqrt(252 * 24 * 60) 

    def calculate_gini(self):
        """Gini Coefficient to measure PnL inequality (0 = steady wins, 1 = extreme variance)"""
        if len(self.pnl_history) < 3: return 0.0
        sorted_pnl = sorted([abs(p) for p in self.pnl_history])
        n = len(sorted_pnl)
        cum_pnl = sum(sorted_pnl)
        if cum_pnl == 0: return 0.0
        gini = (2.0 * sum((i + 1) * p for i, p in enumerate(sorted_pnl)) / (n * cum_pnl)) - ((n + 1) / n)
        return gini

    def process_messages(self):
        while True:
            try:
                msg = self.socket.recv_string(flags=zmq.NOBLOCK)
                data = json.loads(msg)
                msg_type = data.get("type")
                
                if msg_type == "tick": self.latest_tick = data
                elif msg_type == "bar": self.latest_bar = data
                elif msg_type == "stats": self.latest_stats = data
                elif msg_type == "signal":
                    self.recent_signals.appendleft(data)
                    # ---- SIMULATED PNL FILL FOR METRICS ----
                    # In reality, the C++ Engine sends a "Trade Closed" event here.
                    self.total_trades += 1
                    # Simulate 60% win rate for dashboard testing
                    is_win = (self.total_trades % 5 != 0) 
                    if is_win:
                        self.winning_trades += 1
                        sim_pnl = data.get("take_profit", 0) - data.get("entry", 0)
                    else:
                        sim_pnl = data.get("stop_loss", 0) - data.get("entry", 0)
                    
                    self.current_equity += abs(sim_pnl) * (1 if is_win else -1)
                    self.peak_equity = max(self.peak_equity, self.current_equity)
                    self.pnl_history.append(self.current_equity)
                    
            except zmq.Again:
                break 
            except json.JSONDecodeError:
                pass

    def generate_layout(self) -> Layout:
        layout = Layout()
        layout.split_column(Layout(name="header", size=3), Layout(name="main"))
        layout["main"].split_row(Layout(name="left_pane", ratio=1), Layout(name="right_pane", ratio=2))
        layout["left_pane"].split_column(Layout(name="market_data"), Layout(name="engine_stats"))
        layout["right_pane"].split_column(Layout(name="signals", ratio=2), Layout(name="portfolio", ratio=1))
        return layout

    def render_portfolio(self) -> Panel:
        table = Table(box=box.SIMPLE, show_header=False, expand=True)
        table.add_column("Metric", style="cyan")
        table.add_column("Value", justify="right")
        
        drawdown = ((self.peak_equity - self.current_equity) / self.peak_equity) * 100
        win_rate = (self.winning_trades / self.total_trades * 100) if self.total_trades > 0 else 0
        sharpe = self.calculate_sharpe()
        gini = self.calculate_gini()
        
        pnl_color = "green" if self.current_equity >= 100000 else "red"
        
        table.add_row("Total Capital", f"[{pnl_color}]${self.current_equity:,.2f}[/]")
        table.add_row("Max Drawdown", f"{drawdown:.2f}%")
        table.add_row("Win Rate", f"{win_rate:.1f}%")
        table.add_row("Sharpe Ratio", f"{sharpe:.2f}", style="bold yellow")
        table.add_row("Gini Index", f"{gini:.2f}")
        table.add_row("Total Executions", f"{self.total_trades}")

        return Panel(table, title="[bold yellow]Portfolio & Risk Management", border_style="yellow")

    # ... (render_signals, render_market_data, and render_stats remain the same) ...
    def render_market_data(self) -> Panel:
        table = Table(box=box.SIMPLE, show_header=False)
        table.add_column("Key", style="cyan"); table.add_column("Value", justify="right", style="green")
        if self.latest_tick:
            table.add_row("Tick Price", f"{self.latest_tick.get('price', 0):.4f}")
        if self.latest_bar:
            table.add_row("Bar Close", f"{self.latest_bar.get('close', 0):.4f}")
        return Panel(table, title="[bold blue]Market Feed", border_style="blue")
        
    def render_stats(self) -> Panel:
        table = Table(box=box.SIMPLE, show_header=False)
        table.add_column("Metric", style="cyan"); table.add_column("Value", justify="right", style="yellow")
        if self.latest_stats:
            table.add_row("Ticks", f"{self.latest_stats.get('tick_count', 0):,}")
            table.add_row("Lat p99", f"{self.latest_stats.get('lat_p99_ns', 0):,} ns", style="bold red")
        return Panel(table, title="[bold magenta]Engine Performance", border_style="magenta")

    def render_signals(self) -> Panel:
        table = Table(box=box.ROUNDED, expand=True)
        table.add_column("Strategy", style="bold cyan"); table.add_column("Entry", justify="right", style="green")
        for sig in self.recent_signals:
            table.add_row(sig.get("signal_type", "UNKNOWN"), f"{sig.get('entry', 0):.4f}")
        return Panel(table, title="[bold green]Latest Trade Signals", border_style="green")

    def run(self):
        layout = self.generate_layout()
        with Live(layout, refresh_per_second=20, screen=True):
            try:
                while True:
                    self.process_messages()
                    layout["header"].update(Panel(Text("HFT Signal Engine", justify="center", style="bold white on blue")))
                    layout["market_data"].update(self.render_market_data())
                    layout["engine_stats"].update(self.render_stats())
                    layout["signals"].update(self.render_signals())
                    layout["portfolio"].update(self.render_portfolio())
                    time.sleep(0.05)
            except KeyboardInterrupt: pass

if __name__ == "__main__":
    dashboard = HFTDashboard()
    dashboard.run()