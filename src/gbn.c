/*
 * gobackn.c
 *
 * Created on: 2014/11/21
 * Author: Yang Lu
 */

#include <stdio.h>			/* for printf() and fprintf() */
#include <stdlib.h>			/* for atoi() and exit() */
#include <string.h>			/* for memset() */
#include <unistd.h>			/* for close() and alarm() */
#include <errno.h>			/* for errno and EINTR */
#include <signal.h>			/* for sigaction() */
#include <semaphore.h>      /* for semaphore */
#include <pthread.h>		/* for pthread_create */
#include <time.h>			/* for clock() */

#include "gbn.h"
#include "common.h"
#include "physical.h"

static int g_windowSize;

static int g_statsPktRecv = 0;
static int g_statsPktSent = 0;

int tries = 0;			/* Count of times sent - GLOBAL for signal-handler access */
int sendflag = 1;

void write_sender_stats(const char* path) {
	FILE *fp = fopen(path, "a");
	if (!fp) {
		perror("create stat file fails");
		return;
	}
	fprintf(fp, "The total number of frames need to transmitted: %d\n", g_gbnStat.actualFrames);
	fprintf(fp, "The total number of data frames transmitted: %d\n", g_gbnStat.frameSent);
	fprintf(fp, "The total number of retransmissions: %d\n", g_gbnStat.frameRetrans);
	fprintf(fp, "The total number of acknowledgements received: %d\n", g_gbnStat.ackRecv);
	fprintf(fp, "The file size: %d\n", g_gbnStat.filesize);
	fprintf(fp, "The total number of bytes sent: %d\n", g_gbnStat.bytesSent);
	fprintf(fp, "The total milliseconds taken to complete the request: %d\n\n\n", g_gbnStat.timeTaken);
	fclose(fp);
}

void write_receiver_stats(const char* path) {
	FILE *fp = fopen(path, "a");
	if (!fp) {
		perror("create stat file fails");
		return;
	}
	fprintf(fp, "The total number of acknowledgements sent: %d\n", g_gbnStat.ackSent);
	fprintf(fp, "The total number of duplicate frames received: %d\n\n\n", g_gbnStat.dupFrameRecv);
	fclose(fp);
}

void
CatchAlarm (int ignored)	/* Handler for SIGALRM */
{
	tries += 1;
	sendflag = 1;
	g_gbnStat.frameRetrans += g_statsPktSent - g_statsPktRecv;
}

void
DieWithError (char *errorMessage)
{
	perror (errorMessage);
	exit (1);
}

int
max (int a, int b)
{
	if (b > a)
		return b;
	return a;
}

int
min(int a, int b)
{
	if(b>a)
		return a;
	return b;
}

void gbn_init(int windowSize, double lossRate, double corruptionRate) {
	g_windowSize = windowSize;
	physical_init(lossRate, corruptionRate);
	memset(&g_gbnStat, 0, sizeof(g_gbnStat));
}

size_t gbn_send(int sockfd, char* buffer, size_t length) {
	struct sigaction timerAction;	/* For setting signal handler */
	int respLen;			/* Size of received datagram */
	int pkt_recv = -1;	/* highest ack received */
	int pkt_send = -1;		/* highest packet sent */
	int nPackets = 0;		/* number of packets to send */
	int base = 0;
	int bytesSent = 0;

	nPackets = length / CHUNKSIZE;
	if (length % CHUNKSIZE) {
		nPackets++;			/* if it doesn't divide cleanly, need one more odd-sized packet */
	}
	g_gbnStat.actualFrames = nPackets;
	g_gbnStat.filesize = length;

	/* Set signal handler for alarm signal */
	timerAction.sa_handler = CatchAlarm;
	/* block everything in handler */
	if (sigfillset (&timerAction.sa_mask) < 0) {
		DieWithError ("sigfillset() failed");
	}
	timerAction.sa_flags = 0;
	if (sigaction (SIGALRM, &timerAction, 0) < 0) {
		DieWithError ("sigaction() failed for SIGALRM");
	}

	clock_t start = clock(), diff;
	/* Send the string to the server */
	while ((pkt_recv < nPackets-1) && (tries < MAXTRIES))
	{
		// printf ("in the send loop base %d packet_sent %d packet_received %d\n",
		//      base, packet_sent, packet_received);
		if (sendflag > 0)	{
			sendflag = 0;
			int ctr; /*window size counter */
			for (ctr = 0; ctr < g_windowSize; ctr++)
			{
				pkt_send = min(max (base + ctr, pkt_send),nPackets-1); /* calc highest packet sent */
				Frame_t currFrame; /* current packet we're working with */
				if ((base + ctr) < nPackets)
				{
					memset(&currFrame,0,sizeof(currFrame));
					printf ("sending packet %d packet_sent %d packet_received %d\n",
							base+ctr, pkt_send, pkt_recv);

					currFrame.pkt.type = htonl (PACKET); /*convert to network endianness */
					currFrame.pkt.seq_no = htonl (base + ctr);

					int currlength;
					if ((length - ((base + ctr) * CHUNKSIZE)) >= CHUNKSIZE) /* length chunksize except last packet */
						currlength = CHUNKSIZE;
					else
						currlength = length % CHUNKSIZE;
					bytesSent += currlength;
					g_gbnStat.bytesSent += currlength;
					currFrame.pkt.length = htonl (currlength);

					memcpy (currFrame.pkt.data, /*copy buffer data into packet */
							buffer + ((base + ctr) * CHUNKSIZE), currlength);

					currFrame.checksum = htonl (calcChecksum(&(currFrame.pkt)));
//					printf("sending checksum=%u host-order: %u\n", currFrame.checksum, ntohl(currFrame.checksum));
//					printf("currFrame(type=%d, seq_no=%d, data={%d}, length=%d)\n",
//							currFrame.pkt.type, currFrame.pkt.seq_no, currFrame.pkt.data[0], currFrame.pkt.length);
//					printf("send size: %d, actual size:%d\n", FRAME_HEADER + currlength, sizeof(currFrame));
					if (physical_send(sockfd, &currFrame, FRAME_HEADER + currlength) < 0) {
						DieWithError
						("physical_send() sent a different number of bytes than expected");
					}
				}
			}
		}
		/* Get a response */

		alarm (TIMEOUT);	/* Set the timeout */
		Frame_t currAck;
		respLen = physical_recv (sockfd, &currAck, sizeof(currAck));
		while (respLen < 0) {
			if (errno == EINTR)	/* Alarm went off  */
			{
				if (tries < MAXTRIES)	/* incremented by signal handler */
				{
					printf ("timed out, %d more tries...\n", MAXTRIES - tries);
					break;
				}
				else
					DieWithError ("No Response");
			}
			else
				DieWithError ("physical_recv failed");
		}

		/* recvfrom() got something --  cancel the timeout */
		if (respLen) {
			int acktype = ntohl (currAck.pkt.type); /* convert to host byte order */
			int ackno = ntohl (currAck.pkt.seq_no);
			unsigned int checksum = ntohl (currAck.checksum);

			// debug message
			switch (acktype) {
			case PACKET:
				printf("receive PACKET %d checksum=%u\n", ackno, checksum);
				break;
			case ACK_MSG:
				printf("receive ACK %d checksum=%u\n", ackno, checksum);
				break;
			case TEARDOWN:
				printf("receive TEARDOWN %d checksum=%u\n", ackno, checksum);
				break;
			default:
				printf("receive unknown frame\n");
				break;
			}

			// TODO: if checksum is not match the data, discard the ACK
			g_gbnStat.ackRecv++;

			if (ackno > pkt_recv && acktype == ACK_MSG) {
				printf ("received in-order ACK %d\n", ackno); /* receive/handle ack */
				pkt_recv = ackno;
				base = pkt_recv + 1; /* handle new ack */
				g_statsPktRecv = pkt_recv;
				g_statsPktSent = pkt_send;
				if (pkt_recv == pkt_send) { /* all sent packets acked */
					alarm (0); /* clear alarm */
					tries = 0;
					sendflag = 1;
				}
				else { /* not all sent packets acked */
					tries = 0; /* reset retry counter */
					sendflag = 0;
					alarm(TIMEOUT); /* reset alarm */
				}
			}
		}
	}
	int ctr;
	for (ctr = 0; ctr < 1; ctr++) /* send 1 teardown packets - don't have to necessarily send 10 but spec said "up to 10" */
	{
		Frame_t teardown;
		teardown.pkt.type = htonl (TEARDOWN);
		teardown.pkt.seq_no = htonl (0);
		teardown.pkt.length = htonl (0);
		teardown.checksum = htonl (calcChecksum(&(teardown.pkt)));
		while (1) {
			int bytesSent = physical_send (sockfd, &teardown, sizeof(teardown));
			printf("send out teardown msg\n");
			if (bytesSent == sizeof(teardown)) {
				printf("teardown msg reach the other side\n");
				break;
			}
		}
	}
	diff = clock() - start;
	int msec = diff * 1000 / CLOCKS_PER_SEC;
	g_gbnStat.timeTaken = msec;

	return bytesSent;
}


size_t gbn_recv(int sockfd, char* buffer) {
	int recvMsgSize = 0;		/* Size of received message */
	int packet_rcvd = -1;		/* highest packet successfully received */
	struct sigaction myAction;

	while (1) {
		Frame_t currFrame;
		memset(&currFrame, 0, sizeof(currFrame));
		/* Block until receive message from a client */
		//while ((recvMsgSize = physical_recv (sockfd, &currPacket, sizeof (currPacket))) > 0) {

		if (physical_recv (sockfd, &currFrame, sizeof (currFrame)) == 0) {
			continue; // corruption detected
		}

		currFrame.pkt.type = ntohl (currFrame.pkt.type);
		currFrame.pkt.length = ntohl (currFrame.pkt.length); /* convert from network to host byte order */
		currFrame.pkt.seq_no = ntohl (currFrame.pkt.seq_no);
		if (currFrame.pkt.type == TEARDOWN) /* tear-down message */
		{
			// assume tear-down message is always received
			printf("Receive a teardown msg\n");
			return recvMsgSize;
//			printf ("%s\n", buffer);
//			Packet_t ackmsg;
//			ackmsg.type = htonl(8); // ack of tear-down
//			ackmsg.seq_no = htonl(0);/*convert to network endianness */
//			ackmsg.length = htonl(0);
//			if (physical_send (sockfd, &ackmsg, sizeof (ackmsg)) != sizeof (ackmsg))
//			{
//				DieWithError ("Error sending tear-down ack"); /* not a big deal-data already rcvd */
//			}
//			while (1)
//			{
//				physical_send (sockfd, &currPacket, sizeof (int)*3 + CHUNKSIZE);
//				if (ntohl(currPacket.type) == 4) /* respond to more teardown messages */
//				{
//					ackmsg.type = htonl(8);
//					ackmsg.seq_no = htonl(0);/* convert to network endianness */
//					ackmsg.length = htonl(0);
//					if (physical_send (sockfd, &ackmsg, sizeof (ackmsg)) != sizeof (ackmsg))
//					{
//						DieWithError ("Error sending tear-down ack");
//					}
//				}
//			}
//			DieWithError ("physical_send() failed");
		} else if (currFrame.pkt.type == PACKET) {
			printf ("---- RECEIVE PACKET %d length %d\n", currFrame.pkt.seq_no, currFrame.pkt.length);

			/* Send ack and store in buffer */
			if (currFrame.pkt.seq_no == packet_rcvd + 1) {
				packet_rcvd++;
				int buff_offset = CHUNKSIZE * currFrame.pkt.seq_no;
				memcpy (&buffer[buff_offset], currFrame.pkt.data, /* copy packet data to buffer */
						currFrame.pkt.length);
				recvMsgSize += currFrame.pkt.length;
			} else {
				g_gbnStat.dupFrameRecv++;
			}
			printf ("---- SEND ACK %d\n", packet_rcvd);
			Frame_t currAck;
			currAck.pkt.type = htonl (ACK_MSG); /*convert to network byte order */
			currAck.pkt.seq_no = htonl (packet_rcvd);
			currAck.pkt.length = htonl(0);
			currAck.checksum = htonl(calcChecksum(&(currAck.pkt)));
			if (physical_send (sockfd, &currAck, sizeof(currAck)) != sizeof(currAck)) {
				printf("ack msg %d loss\n", packet_rcvd);
			}
			g_gbnStat.ackSent++;
		}
	}

	return recvMsgSize;
}


