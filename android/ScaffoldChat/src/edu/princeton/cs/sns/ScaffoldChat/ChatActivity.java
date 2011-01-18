package edu.princeton.cs.sns.ScaffoldChat;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.SocketException;

import edu.princeton.cs.sns.ScaffoldChat.R;
import edu.princeton.cs.sns.scaffold.*;
import android.app.Activity;
import android.os.Bundle;
import android.text.Editable;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ListView;
import android.widget.TextView;

public class ChatActivity extends Activity {
    /** Called when the activity is first created. */
	private TextView chatWindow = null;
	private TextView statusText = null;
	private EditText chatInput = null;
	private Button sendButton = null;
	private Button cancelButton = null;
	private ScaffoldDatagramSocket sock = null;
    private ListView mConversationView;
    private ArrayAdapter<String> mConversationArrayAdapter;
	
    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);
        
        chatInput = (EditText) findViewById(R.id.chatinput);
        statusText = (TextView) findViewById(R.id.statuslabel);
        chatWindow = (TextView) findViewById(R.id.chatwindow);
        sendButton = (Button) findViewById(R.id.send);
        cancelButton = (Button) findViewById(R.id.cancel);
        
        // Initialize the array adapter for the conversation thread
        //mConversationArrayAdapter = new ArrayAdapter<String>(this, R.layout.message);
        //mConversationView = (ListView) findViewById(R.id.chatlist);
        //mConversationView.setAdapter(mConversationArrayAdapter);

        chatInput.setOnKeyListener(new View.OnKeyListener() {
			public boolean onKey(View v, int keyCode, KeyEvent event) {
				switch (keyCode) {
				case KeyEvent.KEYCODE_ENTER:
					if (chatInput.hasFocus()) {
						sendText();
					}
					return true;
				}
				return false;
			}
        });
        sendButton.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
            	sendText();
            }
        });
        cancelButton.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
            	cancelSend();
            }
        });
    }
    @Override
	protected void onStart() {
		super.onStart();
		Log.d("ScaffoldChat", "onStart");
		
		try {
			sock = new ScaffoldDatagramSocket();
			sock.bind(new ScaffoldSocketAddress(new ServiceId((short) 32769)));
			sock.connect(new ScaffoldSocketAddress(new ServiceId((short) 16385)));
			Log.d("ScaffoldChat", "connected");
			statusText.setText("Connected");
		} catch (SocketException e) {
			Log.d("ScaffoldChat", "Error: " + e.getMessage());
			sock = null;
		}
    }
    @Override
	protected void onStop() {
		super.onStop();
		Log.d("ScaffoldChat", "onStop");
		if (sock != null)
			sock.close();
    }
    @Override
	protected void onDestroy() {
		super.onDestroy();
		Log.d("ScaffoldChat", "onDestroy");
	}
    private void sendText() {
		Editable ed = chatInput.getText();
		String msg = ed.toString();

        this.chatWindow.append("me: " + msg + "\n");
        //mConversationArrayAdapter.add("Me:  " + msg);
        
		ed.clear();
		
		if (sock != null) {
			byte[] data = msg.getBytes();
			
			try {
				DatagramPacket pack = new DatagramPacket(data, data.length);
				sock.send(pack);
				// FIXME: Should not do a blocking receive in this function
				sock.receive(pack);

				String rsp = new String(pack.getData(), 0, pack.getLength());
				Log.d("ScaffoldChat", "response length=" + pack.getLength());
				Log.d("ScaffoldChat", rsp);
		        //mConversationArrayAdapter.add("Other: " + rsp);
				chatWindow.append("Other: " + rsp + "\n");
			} catch (IOException e) {
				// TODO Auto-generated catch block
				e.printStackTrace();
				Log.d("ScaffoldChat", "Error: " + e.getMessage());
				if (sock != null) {
					sock.close();
					sock = null;
				}
				//msg += " - failed!";
			}
		}

    }
    private void cancelSend() {
    	Editable ed = chatInput.getText();
    	ed.clear();
    }
}