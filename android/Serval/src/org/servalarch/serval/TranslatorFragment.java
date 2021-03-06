/* -*- Mode: Java; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
package org.servalarch.serval;

import java.io.File;
import java.io.IOException;
import java.io.FileNotFoundException;
import java.io.InputStreamReader;
import java.io.FileInputStream;
import java.io.BufferedReader;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityManager.RunningServiceInfo;
import android.content.Intent;
import android.os.Bundle;
import android.support.v4.app.Fragment;
import android.text.Html;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.View.OnClickListener;
import android.view.ViewGroup;
import android.view.animation.AlphaAnimation;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.TextView;
import android.widget.ToggleButton;

public class TranslatorFragment extends Fragment {

	private static final String[] ADD_HTTP_RULES = {
		"ifconfig dummy0 192.168.25.25 arp off",
		"ip rule add fwmark 0x4 table main prio 10",
		//"ip rule add to 128.112.7.54 table main priority 10", // TODO this change based on the proxy IP
		"ip rule add from 192.168.25.0/24 table main priority 20",
		"ip rule add from all table 1 priority 30",
		"ip route add default via 192.168.25.26 dev dummy0 table 1",
		"echo 1 > /proc/sys/net/ipv4/ip_forward",
		"echo 2 > /proc/sys/net/ipv4/conf/all/rp_filter",
		"echo 1024 > /proc/sys/net/ipv4/neigh/default/gc_thresh1",
		"echo 2048 > /proc/sys/net/ipv4/neigh/default/gc_thresh2",
		"echo 4096 > /proc/sys/net/ipv4/neigh/default/gc_thresh3",
		
		"iptables -t nat -N serval_OUTPUT",
		"iptables -t nat -A serval_OUTPUT -p tcp --dport 80 -m tcp --syn -j REDIRECT --to-ports 8080",
		"iptables -t nat -A serval_OUTPUT -p tcp --dport 443 -m tcp --syn -j REDIRECT --to-ports 8080",
		"iptables -t nat -A serval_OUTPUT -p tcp --dport 5001 -m tcp --syn -j REDIRECT --to-ports 8080",
		"iptables -t nat -A serval_OUTPUT -j RETURN",
		"iptables -t nat -I OUTPUT 1 -j serval_OUTPUT",
		
		"iptables -N serval_FORWARD",
		"iptables -A serval_FORWARD -s 192.168.25.0/255.255.255.0 -p tcp --dport 80 -j DROP",
		"iptables -A serval_FORWARD -s 192.168.25.0/255.255.255.0 -p tcp --dport 443 -j DROP",
		"iptables -A serval_FORWARD -s 192.168.25.0/255.255.255.0 -p tcp --dport 5001 -j DROP",
		"iptables -A serval_FORWARD -s 192.168.25.0/255.255.255.0 -j RETURN",
		"iptables -I FORWARD 1 -j serval_FORWARD",
		
		"iptables -t mangle -N serval_OUTPUT",
		"iptables -t mangle -A serval_OUTPUT ! -p tcp -j MARK --set-mark 0x4",
		"iptables -t mangle -A serval_OUTPUT -j RETURN",
		"iptables -t mangle -I OUTPUT 1 -j serval_OUTPUT",
		
		"iptables -t nat -N serval_POSTROUTING",
		"iptables -t nat -A serval_POSTROUTING ! -o dummy0 -j MASQUERADE",
		"iptables -t nat -A serval_POSTROUTING -j RETURN",
		"iptables -t nat -I POSTROUTING 1 ! -o lo -j serval_POSTROUTING"
	};
	
	private static final String[] ADD_ALL_RULES = {
		"iptables -t nat -A OUTPUT -p tcp -m tcp --syn -j REDIRECT --to-ports 8080"
	};
	
	private static final String[] DEL_HTTP_RULES = {
		"ifconfig dummy0 down",
		"ip rule del fwmark 0x4 table main prio 10",
		//"ip rule del to 128.112.7.54 table main priority 10", // TODO this change based on the proxy IP
		"ip rule del from 192.168.25.0/24 table main priority 20",
		"ip rule del from all table 1 priority 30",
		"echo 0 > /proc/sys/net/ipv4/ip_forward",
		"echo 0 > /proc/sys/net/ipv4/conf/all/rp_filter",
		"iptables -t nat -F serval_OUTPUT",
		"iptables -t nat -D OUTPUT -j serval_OUTPUT",
		"iptables -t nat -X serval_OUTPUT",
		
		"iptables -F serval_FORWARD",
		"iptables -D FORWARD -j serval_FORWARD",
		"iptables -X serval_FORWARD",
		
		"iptables -t mangle -F serval_OUTPUT",
		"iptables -t mangle -D OUTPUT -j serval_OUTPUT",
		"iptables -t mangle -X serval_OUTPUT",
		
		"iptables -t nat -F serval_POSTROUTING",
		"iptables -t nat -D POSTROUTING ! -o lo -j serval_POSTROUTING",
		"iptables -t nat -X serval_POSTROUTING"
	};
	
	private static final String[] DEL_ALL_RULES = {
		"iptables -t nat -D OUTPUT -p tcp -m tcp --syn -j REDIRECT --to-ports 8080"
	};
	
	private static boolean dummyTested = false;
	
	private Button translatorButton;
	private ToggleButton transHttpButton, transAllButton, hijackButton;
	private View view;

	public View onCreateView(LayoutInflater inflater, ViewGroup container,
			Bundle savedInstanceState) {
		super.onCreateView(inflater, container, savedInstanceState);
		view = inflater.inflate(R.layout.frag_translator, container, false);

		this.translatorButton = (Button) view.findViewById(R.id.translator_toggle);
		this.translatorButton.setOnClickListener(new OnClickListener() {
			@Override
			public void onClick(View view) {
				if (!view.isSelected()) {
					ServalActivity act = (ServalActivity) getActivity();
					boolean success = testDummyIface();

					if (!success) {
						// No dummy interface, try to load module
						act.loadKernelModule("dummy");
						Log.d("Serval", "dummy load: " + success);
						success = testDummyIface();
					}

					if (success) {
						String[] rules = transAllButton.isChecked() ? ADD_ALL_RULES
								: ADD_HTTP_RULES;
						if (executeRules(rules)) {
							getActivity().startService(
									new Intent(getActivity(),
											TranslatorService.class));
							setTranslatorButton(true);
						}
					} else {
						((TextView) getView().findViewById(R.id.error_msg))
								.setText(getString(R.string.no_dummy));
						translatorButton.setClickable(false);
						AlphaAnimation anim = new AlphaAnimation(1.0f, .6f);
						anim.setFillAfter(true);
						translatorButton.startAnimation(anim);
					}
				} else {
					setTranslatorButton(false);
					getActivity().stopService(
							new Intent(getActivity(), TranslatorService.class));
					if (transAllButton.isChecked())
						executeRules(DEL_ALL_RULES);
					else if (transHttpButton.isChecked())
						Log.d("TransFrag", "Deleting Rules");
					executeRules(DEL_HTTP_RULES);
				}
			}
		});

		this.transHttpButton = (ToggleButton) view
				.findViewById(R.id.toggle_trans_http);
		this.transHttpButton
				.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {

					@Override
					public void onCheckedChanged(CompoundButton buttonView,
							boolean isChecked) {
						if (isChecked) {
							if (isTranslatorRunning()
									&& !transAllButton.isChecked()) {
								executeRules(ADD_HTTP_RULES);
							}
						} else {
							if (isTranslatorRunning()
									&& !transAllButton.isChecked()) {
								executeRules(DEL_HTTP_RULES);
							}
						}
					}
				});

		this.transAllButton = (ToggleButton) view
				.findViewById(R.id.toggle_trans_all);
		this.transAllButton
				.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {

					@Override
					public void onCheckedChanged(CompoundButton buttonView,
							boolean isChecked) {
						if (isChecked) {
							if (isTranslatorRunning()) {
								if (transHttpButton.isChecked()) {
									executeRules(DEL_HTTP_RULES);
								}
								executeRules(ADD_ALL_RULES);
							}

						} else {
							if (isTranslatorRunning()) {
								executeRules(DEL_ALL_RULES);
								if (transHttpButton.isChecked()) {
									executeRules(ADD_HTTP_RULES);
								}
							}
						}
					}
				});
		
		this.hijackButton = (ToggleButton) view
				.findViewById(R.id.toggle_inet_hijack);
		
		this.hijackButton
		.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {

			@Override
			public void onCheckedChanged(CompoundButton buttonView,
					boolean isChecked) {
				
				String cmd = "echo " + (isChecked ? "1" : "0") + 
						" > /proc/sys/net/serval/inet_to_serval";
				executeSuCommand(cmd, false);
			}
		});
		
		this.hijackButton
			.setChecked(ServalActivity.readBooleanProcEntry("/proc/sys/net/serval/inet_to_serval"));
		setTranslatorButton(isTranslatorRunning());
		((TextView) view.findViewById(R.id.error_msg)).setText("");
		translatorButton.setClickable(true);

		return view;
	}

	private void setTranslatorButton(boolean running) {
		CharSequence text = Html
				.fromHtml(getString(running ? R.string.translator_running
						: R.string.translator_off));
		translatorButton.setSelected(running);
		translatorButton.setText(text);
	}

	private boolean testDummyIface() {
		File dev = new File("/proc/net/dev");

		if (dev.exists() && dev.canRead()) {
			try {
				BufferedReader in = new BufferedReader(new InputStreamReader(
						new FileInputStream(dev)));

				String line;
				while ((line = in.readLine()) != null) {
					if (line.contains("dummy")) {
						in.close();
						return true;
					}
				}
				in.close();
			} catch (FileNotFoundException e) {
				e.printStackTrace();
			} catch (IOException e) {
				e.printStackTrace();
			}
		} else {
			Log.d("Serval", "could not open /proc/net/dev");
		}
		return false;
	}

	private boolean executeRules(String[] rules) {
		boolean success = true;
		for (String rule : rules) {
			success = success && executeSuCommand(rule, true);
			if (!success)
				return false;
		}
		return success;
	}

	private boolean executeSuCommand(String cmd, boolean showToast) {
		return ((ServalActivity) getActivity())
				.executeSuCommand(cmd, showToast);
	}

	private boolean isTranslatorRunning() {
		ActivityManager manager = (ActivityManager) getActivity()
				.getSystemService(Activity.ACTIVITY_SERVICE);
		for (RunningServiceInfo service : manager
				.getRunningServices(Integer.MAX_VALUE)) {
			if ("org.servalarch.serval.TranslatorService"
					.equals(service.service.getClassName())) {
				return true;
			}
		}
		return false;
	}
}
