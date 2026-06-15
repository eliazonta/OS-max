#!/usr/bin/env python3
import os
import mmap
import json
import time
import math
import struct
import statistics
from collections import deque
from rich.live import Live
from rich.table import Table
from rich.layout import Layout
from rich.panel import Panel
from rich.text import Text
from rich import box

class HFTDashboard:
    def __init__(self, filepath="/tmp/hft_telemetry.mmap"):
        self.filepath = filepath
        self.fd = None
        self.mmap_obj = None
        self.local_tail = 0
        self.buffer_size = (1024 * 1024 * 16) - 8
        
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

    def connect_mmap(self):
        while not os.path.exists(self.filepath):
            time.sleep(0.1)
        self.fd = os.open(self.filepath, os.O_RDONLY)
        self.mmap_obj = mmap.mmap(self.fd, 0, prot=mmap.PROT_READ)
        self.local_tail = struct.unpack('<Q', self.mmap_obj[0:8])[0]

    def process_messages(self):
        PRICE_SCALE = 1_000_000.0
        if not self.mmap_obj: return
        
        try:
            head = struct.unpack('<Q', self.mmap_obj[0:8])[0]
            
            # Prevent overrun: if we are too far behind, skip to head
            if head - self.local_tail > self.buffer_size:
                self.local_tail = head
                
            while self.local_tail < head:
                offset = self.local_tail % self.buffer_size
                
                frame_len_bytes = self.mmap_obj[8 + offset : 8 + offset + 2]
                if len(frame_len_bytes) < 2:
                    break
                frame_len = struct.unpack('<H', frame_len_bytes)[0]
                
                if frame_len == 0:
                    pad = self.buffer_size - offset
                    self.local_tail += pad
                    continue
                    
                msg = self.mmap_obj[8 + offset + 2 : 8 + offset + 2 + frame_len]
                self.local_tail += 2 + frame_len

                if not msg: continue
                kind = msg[0]
                
                if kind == 0 and len(msg) == 33: # TICK
                    _, price, bid, ask, ts_ns = struct.unpack('<BqqqQ', msg)
                    self.latest_tick = {
                        "type": "tick", "price": price/PRICE_SCALE, 
                        "bid": bid/PRICE_SCALE, "ask": ask/PRICE_SCALE, "ts_ns": ts_ns
                    }
                elif kind == 1 and len(msg) == 45: # BAR
                    _, o, h, l, c, vol, ts = struct.unpack('<BqqqqIQ', msg)
                    self.latest_bar = {
                        "type": "bar", "open": o/PRICE_SCALE, "high": h/PRICE_SCALE,
                        "low": l/PRICE_SCALE, "close": c/PRICE_SCALE, "volume": vol, "ts": ts
                    }
                elif kind == 2 and len(msg) == 35: # SIGNAL
                    _, sig_type_id, strength, entry, sl, tp, lag_ns = struct.unpack('<BBBqqqQ', msg)
                    
                    sig_names = ["NONE", "LONG_EMA_CROSS", "SHORT_EMA_CROSS", "LONG_RSI_OVERSOLD", "SHORT_RSI_OVERBOUGHT", "LONG_VWAP_RECLAIM", "SHORT_VWAP_BREAK", "LONG_MOMENTUM", "SHORT_MOMENTUM"]
                    sig_name = sig_names[sig_type_id] if sig_type_id < len(sig_names) else "UNKNOWN"
                    
                    data = {
                        "type": "signal", "signal_type": sig_name, "strength": strength,
                        "entry": entry/PRICE_SCALE, "sl": sl/PRICE_SCALE, "tp": tp/PRICE_SCALE, "lag_ns": lag_ns
                    }
                    self.recent_signals.appendleft(data)
                    
                elif kind == 3 and len(msg) == 65: # STATS
                    _, t_count, s_count, l_avg, l_min, l_p99, pnl, trades, wins = struct.unpack('<BQQQQQqQQ', msg)
                    self.latest_stats = {
                        "type": "stats", "tick_count": t_count, "signal_count": s_count,
                        "lat_avg_ns": l_avg, "lat_min_ns": l_min, "lat_p99_ns": l_p99
                    }
                    self.total_trades = trades
                    self.winning_trades = wins
                    self.current_equity = 100000.0 + (pnl / PRICE_SCALE)
                    self.peak_equity = max(self.peak_equity, self.current_equity)
                    self.pnl_history.append(self.current_equity)
                    
        except Exception as e:
            with open("/tmp/dash_err.log", "a") as f:
                import traceback
                f.write(traceback.format_exc() + "\n")

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
        self.connect_mmap()
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